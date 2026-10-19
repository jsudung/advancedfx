# CS2 mirv_pov spike: global local-player swap (approach B)

Branch `cs2-mirv-pov`. Code: `AfxHookSource2/MirvPov.cpp` (was `MirvPovSpike.cpp` during the spike). Analysed on CS2 1.41.9.1
(ClientVersion 2000933, 2026-10-15). RVAs are orientation only; the code finds everything
by signature and reads offsets out of the matched instructions.

## Findings (static analysis)

### The local player controller (client.dll)

- `GetSplitScreenPlayer(int slot)` (`0x96a8b0`, already located by HLAE in `main.cpp`)
  returns `g_LocalPlayerControllers[slot]` (`0x253cb78`). CS2 has **one** slot (the loops
  are bounded to 1). `GetSplitScreenPlayerPawn` (`0x96a8f0`) is the same array → controller
  → `m_hPawn`. These have 106 and 262 direct callers, which is how most "local player" code
  reaches it. The array itself is only referenced 11 times.
- The only writer of the array is the **local player controller setter** (`0x933de0`). It's
  called only from `CBasePlayerController`'s data-update virtual (`0x958800`, vtable index
  11 of a sub-object), with slot 0:
  1. `slot = engine->vtable[0x110](&tmp, 0)` gives the engine's local player slot (from
     ServerInfo).
  2. **If `g_KillCamObsMode` (`0x253d240`) != 0, then `slot = g_KillCamTarget`
     (`0x253d244`) - 1.**
  3. If `slot == entindex(this) - 1`: `g_LocalPlayerControllers[0] = this`,
     `this->m_bIsLocalPlayerController (+0x790) = true`, `+0x6cc = slot`, then
     local-player-changed notifications.
  4. It never clears the flag on the previous controller.
- Both killcam globals are written only by the `CCSUsrMsg_KillCam` user-message handler
  (`0xcfeec0`, bound via `CGameMessageDelegateHook<CCSUsrMsg_KillCam_t>`): mode =
  `obs_mode`, target = `first_target`. It also toggles a byte at `+0xcb` on another client
  object (`0x22362f8`). `0xcc0520` resets both globals to 0, and two other functions
  (`0xc74ec0`, `0xb9c970`) branch on the killcam mode.
- **This means CS2 still ships CS:GO's killcam mechanism, which temporarily makes another
  player the local player controller.** That's a game-native version of what CS:GO
  `mirv_pov` did with `SetAsLocalPlayer`.
- The data update only re-runs the setter when an update flag is set or no local
  controller exists. While the killcam override is active, the setter rejects every
  controller except the target, so the original local controller can't take the slot back.

### ServerInfo (engine2.dll)

- `CNetworkGameClient::ProcessServerInfo` (`0x6a280`, string
  `"CL:  CNetworkGameClient::ProcessServerInfo\n"`): `this->m_bIsHLTV (+0x2c3558) =
  msg->is_hltv (+0x89)`, then calls `CNetworkGameClientBase::ProcessServerInfo`
  (`0x84810`). That function reads `msg->player_slot (+0x9c)` (key `"player"`) and pushes
  it into the split-screen slot setup. Other fields used there: `+0x80` protocol, `+0x84`
  server count, `+0x88` is_dedicated, `+0x90` max clients, `+0x98` tick interval.
- Rewriting `+0x89` and `+0x9c` before the original runs is the direct equivalent of CS:GO
  `mirv_pov`.

## What the spike implements

`mirv_pov_spike` (always registered, no effect unless used):

| Command | Method |
|---|---|
| `mirv_pov_spike info` | Prints resolved addresses, the last ServerInfo seen (`is_hltv`, `player_slot`), the engine local slot, the killcam globals, the local controller, and all player controllers with their `m_bIsLocalPlayerController` flag. |
| `mirv_pov_spike killcam <idx> [obsMode=2]` | **M1.** Works while the demo is playing: sets the killcam globals, calls the game's setter on the target, and clears the old controller's flag. Re-applied before every `FrameStageNotify` stage (the game resets the globals). `0` restores and lets the game's own check pick the real local controller again. |
| `mirv_pov_spike serverinfo <idx> [isHltv=0]` | **M2.** CS:GO style. Applies on the next demo load: rewrites `player_slot = idx - 1` and `is_hltv`. `0` disables. |

Safety: everything is resolved by signature and verified bytes. If something doesn't
match, the command reports "not found" and does nothing. Swap and restore run under SEH;
on an exception M1 disables itself.

## Building and installing the spike DLL

Prerequisites: VS 2022 (Build Tools are enough) with C++ and the .NET 4.6.2 targeting pack,
CMake ≥ 3.24, Rust stable MSVC, and Node.js. From the repository root:

```bat
cmake --preset x64-release
cmake --build build/x64-release --config Release --target AfxHookSource2
```

Output: `build/x64-release/AfxHookSource2/Release/AfxHookSource2.dll` (+ `.pdb`).

Notes:
- The root `CMakeLists.txt` now passes `-products *` to `vswhere`. Without it a Build
  Tools-only install isn't found, and configure fails in `string(REPLACE ...)`.
- If `NoDefaultCurrentDirectoryInExePath` is set in your environment, the shader step fails
  with `'ShaderBuilder.exe' is not recognized`. Put `build/x64-release/ShaderBuilder` on
  `PATH` for the build.

Install: set up official HLAE 2.192.8 (this branch is based on `7567c24e`, two fixes after
2.192.8). Close CS2, back up `<HLAE>\x64\AfxHookSource2.dll`, copy the built DLL (and
optionally the PDB) over it, then launch CS2 through HLAE as usual.

## Test protocol (in game)

Use a GOTV demo (a pro match) and, if available, a POV demo you recorded yourself, for
comparison.

1. Launch CS2 through HLAE with the spike DLL. Run `mirv_pov_spike info` in the main menu.
   Note whether both client.dll and engine2.dll are `OK`.
2. `playdemo <demo>`, wait until players are in, then pause (`demo_pause`).
   `mirv_pov_spike info`: note `Last ServerInfo` (`is_hltv`, `player_slot`), the engine
   local slot, which controller is `<- local` (GOTV/SourceTV?) and the target's index.
3. **M1:** `mirv_pov_spike killcam <idx>`, then `demo_resume`. Record:
   - Does the view switch to the target's first person on its own (without `spec_mode`)?
     Viewmodel?
   - HUD: player HUD (health/armour/ammo/money) or spectator HUD? Crosshair? Radar
     teammates/colours? Flash and smoke overlays? Scope? Damage direction?
   - Is the camera smooth, or does it jitter (prediction)? Do the eye angles look right?
   - Behaviour across death, round restart, `demo_gototick` back and forward, switching
     to another `<idx>`, and `killcam 0` restore.
   - Crashes: note the last command and tick.
   - Try `obsMode` 1–5 if 2 does odd things.
4. **M2:** `disconnect`, `mirv_pov_spike killcam 0`, `mirv_pov_spike serverinfo <idx>`,
   then `playdemo <demo>` again. Record the same items, plus whether loading works at all
   and what `info` shows (`Last ServerInfo ... (rewritten by us)`).
   Also try `serverinfo <idx> 1` (keep `is_hltv=1`, change only the slot).
5. Optional: run `mirv_pov_spike info` during M1 and M2 to confirm only the target has the
   local flag.

## Test results so far (CS2 1.41.9.1, GOTV demo, 2026-10-19)

- `info` in a GOTV demo: `is_hltv=1 player_slot=12`, so the local controller is 13
  "DemoRecorder". Players are controllers 1–10.
- **M1 (killcam swap)** works: the game accepts the target as local controller and the
  camera moves to their eyes. The swap persists through round end, death and respawn.
  - The viewmodel and crosshair are missing, and the radar stays the full spectator radar,
    because the client still believes it's HLTV.
  - Seeking with the swap active crashed (client.dll `+0xb30b36`). The chain: pawn
    data update → `C_BasePlayerPawn` vtable[113] → `CCSPlayer_CameraServices` vtable[23]
    → insert into an unallocated hash table (local-player-only camera state). **Fixed** by
    hooking `CDemoPlayer::SkipToTick` (engine2, vtable[19]): restore before the seek,
    re-apply 64 render frames later.
- **M2 (serverinfo rewrite, is_hltv=0, player_slot=5)**: the viewmodel and crosshair
  appear, but the camera sits at a stray spot, the console fills with errors, and then it
  crashes in `FX_FireBullets` (client.dll `+0x943537`, null shooter pawn). Likely cause:
  GOTV demos contain every player's user commands (`CSVCMsg_UserCommands_t ... for slot N`),
  so with `player_slot` changed the engine treats the target's recorded commands as local
  input and the client runs local prediction and shot effects from them. **Parked.** Those
  recorded commands could later give exact view angles.
- **`IsHLTV()` (engine2 `CEngineClient` vtable[86])**: 40 direct call sites in client.dll,
  but `CSource2Client::FrameStageNotify` caches it per frame in a global (`0x25603a0`,
  59 readers). Callers seen during playback:
  `0xb6d75f` (FrameStageNotify cache), `0x943fd0`, `0x9440dd` (hot, `m_bIsLocalPlayerController`
  checks near FX_FireBullets), `0xeaf1c3`, `0xeaf256`, `0xe92a9d` (spectate HUD), `0xe09b5d`,
  `0xec85fb` (radar: map overview), `0xec590b`, `0xec7f3e`, `0xed3299` (team counter).
- **M1 + `hltv 2`** (spoof only the cache): the viewmodel and crosshair appear. The
  crosshair is the viewer's own settings, not the target's (needs the target's crosshair
  code, as CS:GO mirv_pov did). The radar is still the spectator radar because it calls
  `IsHLTV()` directly. The C4 carrier is already coloured as a teammate (local team logic
  follows the target).
- **M1 + `hltv 1`** (spoof all client callers): **works well.** The player radar
  (circular, teammates only, enemies red when spotted, correct colours), player HUD,
  killfeed and death notices are all native. No spam, no crash. All 10 observed callers are
  spoofed.
- **Crosshair:** as local player the HUD calls `ApplyOwnCrosshair(settings)` (client.dll
  `0xdb0230`, copies `cl_crosshair*`). The spectator path `ApplyObserverCrosshair`
  (`0xdb03f0`, `cl_show_observer_crosshair`) ends with
  `ApplyCrosshairCode(settings, GetCrosshairCode(controller))` (`0xdaf490` / `0x889da0`).
  The spike now detours `ApplyOwnCrosshair` while swapped and applies the target's code
  instead. That doesn't touch the viewer's convars, and it falls back if the code is empty or
  invalid. Not yet verified in game.

- **Crosshair:** confirmed working in game (target's crosshair code).
- **Demo UI kill/death markers** follow the target: `killcam` now runs `spec_player <n>` while
  the GOTV recorder is local (8 frame pause), and invalidates the observer services'
  "last target" cache (+0x60) so `spec_target_updated` fires. A restore bug (the loop
  stopped at the still-local swapped controller) had broken direct switches and
  `killcam 0`. Fixed and confirmed.
- **Buy menu blur:** the recorded `C_CSPlayerPawn::m_bIsBuyMenuOpen` (+0x15ea) blurred
  the view. Cleared every frame while swapped. Confirmed.
- **Scoreboard:** TAB is the `showscores` button (bit 33), handled in `CCSGOInput`
  vtable[4] (CreateMove). The scoreboard update skips when
  `IsPlayingDemo() && !g_bIsHLTV_cached`, so it only worked with `hltv 0`. That single `je`
  is now NOPed while swapped. Confirmed working.
- **Chat:** not shown even without the swap when watching with an account that wasn't
  in the match (and offline). Valve MM demos store chat **encrypted**: the scan of
  `match730_..._142.dem` found 0 plain `CUserMessageSayText2` / `TextMsg` messages but 541
  `svc_EncryptedData` (`CSVCMsg_EncryptedData`, id 78) messages. engine2 has "Key to decrypt
  (public) encrypted GOTV messages" settings, and `.dem.info` carries `encryption_key` /
  `encryption_key_pub`. The game gets the key for match participants (online), and then
  chat shows (confirmed by the user with the main account online). This matches Valve's
  patch notes: chat in demos is encrypted with the same rules as CS:GO. Team chat is never
  readable in demo playback, and global chat only when played back via the in-game watch
  UI. **Chat with the POV swap is confirmed working** on a pro demo with unencrypted chat
  (`chatlog` shows deliveries while swapped). Limitation: POV of MM demos can never show
  the player's team chat.

## Effects audit: what the demos contain

Message type counts from both demos (names from client.dll's embedded proto descriptors):
Valve MM `match730_..._142.dem` and pro (StarLadder) `vitality-vs-furia-m1-mirage.dem`.

Player-only messages, **absent in both**: `CS_UM_Damage` (damage-direction arcs),
`CS_UM_Fade`/`UM_Fade`, `UM_Shake`/`CS_UM_Shake`, `CS_UM_RadioText`/`SendAudio`/`RawAudio`,
`CS_UM_AdjustMoney`, `CS_UM_BarTime`, `CS_UM_PostRoundDamageReport`, `CS_UM_HintText`,
`CS_UM_SendLastKillerDamageToClient`, `CS_UM_UpdateScreenHealthBar`. `UM_TextMsg`: MM 0, pro 11.

Present: entity state (`svc_PacketEntities`), `svc_UserCmds` (1.6–2.0M), game events
(20–25k), `GE_FireBullets`, `GE_SosStart/StopSoundEvent`, `CS_UM_WeaponSound`,
`UM_ParticleManager`, decals, `TE_EffectDispatch`. `GE_PlayerBulletHit` MM only (528).
`svc_EncryptedData` MM only (541, chat). `UM_SayText`/`SayText2` pro only. No
`svc_VoiceData` in either.

Expectation: effects driven by entity state (flash whiteout, health/armour/money, death
camera, scope) work natively in POV. Effects driven by player-only messages (damage
arcs, shake, radio, kill-reward text, bar time, round damage report, flash/HE ringing DSP)
can only be rebuilt from game events (as the fork does).

### In-game audit results (user, M1 + hltv 1)

Native and correct: red screen-edge damage flash, flash whiteout, HE tagging slowdown,
killfeed, grenade/radio voice lines, defuse progress bar (planting has none in the real
game), death camera/fade with correct headshot vs. non-headshot timing.

Missing / wrong:
- **AWP scope blur** shows the wrong state (steady while moving, blurry while standing).
  Likely the local player's predicted inaccuracy, which is stale without prediction.
- **Death card** ("X killed you with ..." + avatar) missing, even though `player_death`
  is in the demo. Likely a gate.
- Damage-direction arcs (no `CS_UM_Damage`).
- No chat-box texts at all: kill reward "+$300", radio texts, money messages
  (no `TextMsg` / `RadioText` / `AdjustMoney`).
- Round-end damage report under avatars (no `PostRoundDamageReport`).
- Flash/HE ringing and muffling, HE screen shake.

### AWP scope blur investigation (parked)

- The blur is wrong in all `hltv` modes, so no HLTV check is involved. It appears frozen
  at the state at the last (re-)swap / seek.
- The target pawn becomes predictable as local player (`C_BaseEntity::m_bPredictable` +0x569,
  set via `SetPredictable`, vtable 130). The spike now blocks that (`predict 0`, default) and
  `info` confirms `m_bPredictable=0`, but **the blur is still wrong**. So prediction of the pawn
  isn't the (only) cause.
- `CCSPlayer_CameraServices::m_vClientScopeInaccuracy` (+0x2c0) is registered as an
  interpolated variable (camera services vtable[8]). The scope shader parameters come
  from the view setup (client.dll ~0x11ec870). The likely cause is local-player
  interpolation of that variable. Fixing it needs deeper RE of component variable
  interpolation for the local player. **Parked as a known issue.**

### Death card ("X killed you with ...") — working

Three things were needed (confirmed in game):

1. **Trigger:** the card is shown only by the `CCSUsrMsg_SendLastKillerDamageToClient`
   handler (client.dll ~0xc8d8c0), which reads six damage ints and calls
   `FindHudElement("CCSGO_HudDeathPanel")` → `SetPendingDamage(...)` (~0xe8e3c0). That
   sets pending and calls Show. The message isn't in demos, so the spike calls
   `SetPendingDamage(panel, 0, ...)` itself after the client processed a `player_death`
   whose `userid` controller is the swapped target. **Damage numbers are 0 for now.**
   The game event is inspected *before* `FireEventClientSide` (which frees it). Reading it
   afterwards crashed.
2. **"Victim is me" gate** in `CCSGO_HudDeathPanel`'s `player_death` handler (~0xe89ed0):
   `event->GetPlayerPawn("userid") == GetSplitScreenPlayerPawn(0)`. The getter resolves
   the controller's `m_hPawn` (+0x6bc), which at death is already the observer pawn, while
   the event gives `m_hPlayerPawn`.
3. **Show** (~0xe8aaf0) bails unless that getter returns a player pawn, and the panel's
   per-frame update (~0xe8c2f0, needs visible +0x1a1) reads its death time for the
   slide-in.

For 2 and 3, `GetSplitScreenPlayerPawn` is detoured and returns the target's
`m_hPlayerPawn` only for those three return addresses while swapped. The fork (per its
notes) does the same in spirit: it calls the panel's Show directly and overrides the local
pawn getter at the animation call site.

### Round end damage report (under the avatars) — working

Two client paths exist, selected by the replicated dev convar `sv_dev_damage_use_netvars`
(registered ~0xb46a0, default **true**, value byte +0x58 of the object at ~0x25358d8):

- **Legacy (convar false):** `CCSUsrMsg_PostRoundDamageReport` → `CCSGO_HudTeamCounter`
  handler (~0xec9e90), one message per other player, matched by xuid. Not in demos.
- **Netvars (convar true, current default):** the change callback of
  `CCSPlayerController_DamageServices::m_nSendUpdate` (~0x910890) runs
  `if (cvar && changed && g_pGameRules && g_pGameRules->m_iRoundWinStatus (+0x9ac))` and tail jumps to
  the TeamCounter reporter (~0xebd170, arg `FindHudElement("CCSGO_HudTeamCounter") - 0x20`).
  The reporter takes the local controller (`GetSplitScreenPlayer(0)`, so the swapped
  target), needs team 2/3 (+0x3e7), clears the rows (~0xec1f70), then walks
  `m_pDamageServices` (+0x830) → `m_DamageList` (size +0x48, memory +0x50, 0x78-byte
  `CDamageRecord`s). It reads attacker / victim controller handles at +0x38 / +0x3c,
  `m_flActualHealthRemoved` float +0x68, hits +0x6c and kill type byte +0x75. For each record
  touching the local controller, it fills the matching avatar row (20 rows at +0xf0, stride
  0xe0, panel at +0x10) via ~0xecd690 (`health_removed`/`num_hits` or `return_*`,
  `kill-type-given/taken`). Then it sets `order_index` and dispatches the reveal event for
  each filled row. Kill type indexes `HUD--killtype-*`: 0 none, 1 default, 2 headshot,
  3 blast, 4 burn, 5 slash, 6 shock.

In GOTV demos the damage list is empty (owner-only). Neither test demo has a `round_end`
game event, and the pro demo doesn't have `round_prestart` either (only `round_freeze_end`,
`round_officially_ended` in MM). So:

- Pairs (attacker → victim controller) are collected from `player_hurt` and
  `player_death` all the time, not only while swapped:
  - Health removed is `dmg_health`, capped by the drop in the victim's `health`.
  - Every hurt event counts as a hit.
  - The kill type comes from `weapon` / `headshot`.
- Round end = `m_iRoundWinStatus` going 0 → non-0, polled each rendered frame. After 8
  frames (so the round's last `player_death` has arrived) the target's `m_DamageList` is
  pointed at our records, the reporter is called, and the list is restored. If the list is
  non-empty, nothing is done (e.g. POV demos, where the game shows the report by itself).
- Events are kept for the whole demo, tagged with the round:
  - The round key is `m_totalRoundsPlayed`, minus 1 while `m_iRoundWinStatus != 0`.
    Both are set together when a round ends, so post-round damage stays in its round.
  - Replays after seeking back are dropped by (tick, attacker, victim, health, damage).
  - At publish, the round's events are sorted by tick and actual health removed is
    recomputed from 100.
  - On `begin_new_match`, events up to that tick are dropped, because warmup has the same
    round number (0) as round 1.
  - The log is cleared on level load.
  - Seeks only reset the win status tracking, so the round end we land on doesn't trigger.
  - Without the schema offset this falls back to clearing per round and on seeks.
- First in-game version (cleared on every seek) worked when playing straight through. When
  the user sought back into a round where the POV player was already dead, nothing was
  shown, because the events from before the seek point weren't replayed.

The GOTV layout's `.TeamCounter__Align--Bottom .prdr__root { visibility: collapse; }` (per
the fork's notes) could still hide it; with the IsHLTV spoof the player layout should be
used.

### Demo UI spectated player after seeks

After a seek the swap pauses and `spec_player <target>` is sent each frame, so the demo UI
(spectated player name, kill / death markers) follows the target. A dead player can't be
spectated, though. Seeking to a moment where the target was dead left the GOTV observer on
another player, and the demo UI showed that name (seen in game: a player on the other team).

The demo UI takes it from the client-side game event `spec_target_updated`, fired by
`CCSObserver_ObserverServices` (sender ~0xa70cc0: `SetPlayer("userid", pawn)` via
event vtable +0xe8, `SetEntity("target", GetObserverTarget())` via +0xd0). We hook
`FireEventClientSide` anyway, so while a target is set the event's `target` is overwritten
with the target's `m_hPlayerPawn` entity (it exists while dead) before the listeners run.
The vtable index is read from the sender (`mov rdi, [rcx+disp]` at +0xbf).

### Damage direction arcs

`CCSGO_HudDamageIndicator` (panels `DamageTop/Right/Bottom/Left`) hooks `CCSUsrMsg_Damage`
(handler ~0xe86d80):

- In GOTV mode (cached IsHLTV) it requires the observer to be in-eye on `victim_entindex`.
- Otherwise it needs `GetSplitScreenPlayerPawn(0)` to be an alive player pawn.
- If `amount` (+0x50) > 0 it calls `ShowDirection(this, &inflictor_world_pos (+0x48), pawn)`
  (~0xe7bae0). That function takes the direction from the pawn's origin to the inflictor,
  projects it on the forward / right vectors of the slot's main view angles (~0x256af58),
  and raises the front / back / right / left intensities (this +0x60 / +0x64 / +0x68 /
  +0x6c). The element's think fades them out.

The message isn't in GOTV demos. On the swapped target's `player_hurt` (still alive, damage
> 0, attacker another player) we call `ShowDirection` with
`FindHudElement("CCSGO_HudDamageIndicator") - 0x20`, the target's player pawn and the
attacker pawn's origin. The address comes from the handler's call site. The red screen edge
flash is a separate path (`HUD--health--on-damage`) and already worked.

### Radio and kill reward chat lines

Both come from player-only messages that GOTV demos don't contain:

- `CCSUsrMsg_RadioText` handler (~0x11a7ca0, on the CHudChat singleton): it localizes
  `msg_name` and four params (`#SLOTNAME[n]` becomes the player name; param 3 `auto` is
  checked against a setting) and adds the team prefix `#game_radio_team_prefix_<team>`. It
  calls `ChatPrintf(this, &filter{0,1}, client slot, "%s", text)` (~0x11a5440), then plays
  `HudChat.Message`. Grenade radio uses `#Game_radio_location` (` %s4%s1﹫%s2: %s3`) or
  `#Game_radio` with the texts `#SFUI_TitlesTXT_{Fire,Flashbang,Smoke,Molotov,Incendiary,Decoy}_in_the_hole`.
- `CUserMessageTextMsg` (CHudChatDelegate vtable[13], ~0x11a8dc0) prints HUD_PRINTTALK
  through the same `ChatPrintf` with client -1. Kill reward:
  `#Player_Cash_Award_Killed_Enemy` (` +$%s1 for neutralizing an enemy with the %s2`).
- `ChatPrintf` ignores `this`. In demo playback it skips everything when the
  crosshair/chat hide flag (settings +0x82) is set. It formats, then hands the text to
  `CCSGO_HudVoiceStatus`.

What we do:

- Radio: on `weapon_fire` with a grenade weapon from a player on the target's team
  (including the target). The location comes from the thrower pawn's `m_szLastPlaceName`,
  localized as `#<place>`.
- Kill reward: the target's `player_death` kills are queued. When the target's
  `InGameMoneyServices::m_iAccount` changes in the right direction (up; down for a team
  kill), the change is printed as the amount (split if several kills share one change).
  Queued kills time out after 32 ticks. Weapon names come from a weapon → `#SFUI_WPNHUD_*`
  table, falling back to `..._Generic`.
- Lines are built with `ILocalize::Find` (vtable index read from the handler) and our own
  `%sN` substitution.

### Enemy radio voice lines

GOTV demos contain every agent voice line (`<agent>.<t|ct>_<concept><nn>`, e.g.
`sas.ct_flashbang02`, from `soundevents/vo/agents/*.vsndevts`) as `CMsgSosStartSoundEvent`
(svc 208). A player only gets their own team's, played as radio. In the MM test demo, every
sound from a player pawn without `packed_params` was agent VO (416), and every other pawn
sound had them (19017). Names were checked by decompressing the vsndevts KV3 (LZ4) and
hashing with MurmurHash2, seed `0x53524332`, on the lowercase name.

The message's `_InternalParse` (vtable[9], ~0x5fc020) is detoured. Field offsets come from
`_InternalSerialize` (vtable[11]): has-bits +0x10 (1 `packed_params` +0x18, 2 guid +0x20,
4 hash +0x24, 8 seed +0x28, 0x10 start_time +0x2c, 0x20 `source_entity_index` +0x30).
After parsing, when the message has no `packed_params` and its source is an enemy pawn of the
target, the hash and its has-bit are cleared, so nothing plays. Parsing may run off the main
thread, so the hook only reads a byte table of muted pawn indices, which the main thread
rebuilds every frame.

The radio chat sound: the RadioText handler plays `HudChat.Message` through
`EmitSound(-1, &params)` (~0xc09410) unless `cl_clutch_mode`. We do the same after our radio
lines.

### Spectator x-ray after seeks

Seen in game: after a seek, while swapped, every player glowed through walls (spectator x-ray).
It went off when someone died and came back with the next seek.

Player glow colour comes from a pawn virtual (~0xc7d380). It returns no glow for an alive
local player, otherwise team colour for everyone when "spectator tools" are on (~0xce2420),
else teammates only. Spectator tools are `force_spectator_only_tools ||
(local->m_bIsLocalPlayerController (+0x790) ? cached g_bIsHLTV : local->m_bIsHLTV (+0x6f0)) ||
<local pawn's controller / game mode checks>`.

The glow isn't recomputed every frame, only on events (a listener for `player_death`,
`player_spawn`, `spec_target_updated`, `round_start`, ...). Our seek resume invalidates the
observer's spec target cache during the paused frames, while the GOTV controller is local
again. So `spec_target_updated` refreshes the glow as a spectator, and that state stays until
the next death.

Fix: the spectator tools check is detoured to return false whenever a POV target is set (also
while paused). `mirv_pov 0` invalidates the spec target cache once more, so the refresh brings
x-ray back for normal spectating. `mirv_pov info` prints the check's inputs.

### Ear ringing and C4 screen shake

Both are sent by the server to single players, so they aren't in GOTV demos (read from
server.dll):

- **Flashbang deafen** (~0x21f540, called only from the flashbang detonation, right after
  `player_blind` for each blinded player). The argument is the distance between the flash and
  the player's eyes:

  | Distance | Sound | Control |
  |---|---|---|
  | < 100 | `Flashbang.Ring.Long` | `control.deafenLong` |
  | < 500 | `Flashbang.Ring.Medium` | `control.deafenMedium` |
  | < 1000 | `Flashbang.Ring.Short` | `control.deafenShort` |

  The control is sent as a `CUserMessageAudioParameter` (~0xfe4ba0): `parameter_type 0x92e1d607`,
  `name_hash_code` = the lowercased control name hashed like a string token (MurmurHash2, seed
  `0x31415926`), value 0. It's the muffling mix.
- **HE deafen** (~0x1d8e50, from the player's damage handling): blast damage >= 30 plays
  `Flashbang.Ring.Long` with `control.deafenHE`.
- **No grenade shake.** `CBaseGrenade` vtable[309] (detonate, ~0x8bb700) only calls
  `UTIL_ScreenShake` when vtable[313] (shake amplitude) is non-zero. Every CS grenade projectile
  (HE, flash, smoke, molotov, decoy) returns 0. The 25/150/1/750 shake found first is the
  `shake` console command ("Shake the screen.", ~0xd86c70), not the HE. The first in-game test
  used it and shook far too much.
- **C4 shake** (~0x9f4110, the C4 explosion): `UTIL_ScreenShake(bomb, 3, 150, 0.5, 10000,
  SHAKE_START, no air shake)`. `UTIL_ScreenShake` (~0xfe3fa0) caps the amplitude at 16 and
  skips players who aren't on the ground. `ComputeShakeAmplitude` falls off linearly
  (`amp * (1 - dist / radius)`).

Client side:

- The shake ends in `CViewEffects::Shake(ScreenShake_t *)`: the per-slot array (stride
  0xa8) found through `shake_testpunch`, vtable[4]. `ScreenShake_t` is { command, amplitude,
  frequency, duration, direction[3] }.
- The `CUserMessageAudioParameter` handler (~0xb9ff80) calls the sound system's
  vtable[0x1b8 / 8] with the hash for that parameter type. The client uses the same call for
  `control.DuckReverb`.
- Ring sounds are played with the same `EmitSound(-1, &params)` as the chat sound (2D).

What we do:

- On the target's `player_blind`: distance from the flashbang entity (`entityid`, or the last
  `flashbang_detonate`) to the eye position, then trigger the deafen control and play the ring.
- On the target's HE `player_hurt` with `dmg_health + dmg_armor >= 30`: Long ring plus
  `control.deafenHE`.
- On `bomb_exploded`: if the target pawn is on the ground (`m_fFlags & FL_ONGROUND`), shake
  with the falloff from the `C_PlantedC4` entity.

### Plant / defuse rewards and team income

From server.dll:

- `CCSGameRules::AddTeamAccount(team, awardType, amount, customMsg)` (~0x906380). For each
  player of the team that receives income, it adds the money and prints
  `#Team_Cash_Award_<type>` with the amount. Players that don't receive income get
  `#Team_Cash_Award_No_Income` (or `_Suicide`). Nothing is printed on the last round of a half
  or match, because money resets.
- Award types:

  | Type | Token |
  |---|---|
  | 1 | `T_Win_Bomb` |
  | 2-3 | `Elim_Hostage` |
  | 4 | `Elim_Bomb` |
  | 5-6 | `Win_Time` |
  | 7 | `Win_Defuse_Bomb` |
  | 8 | `Win_Hostage(s)_Rescue` |
  | 9 | `Loser_Bonus` (`_Neg` if <= 0) |
  | 13 | `Planted_Bomb_But_Defused` |
  | 18 | `Bonus_Shorthanded` |

  Round end awards are queued and paid by the queue processor (~0x954ad0, from
  `CCSGameRules::Think`). The processor computes the loss bonus (~0x90b2c0) as
  `cash_team_loser_bonus + cash_team_loser_bonus_consecutive_rounds * min(losses,
  mp_consecutive_loss_max)`, using the counter from before this round, then updates
  `m_iNumConsecutive{CT,Terrorist}Loses` (the winner's goes down by one, depending on a mode
  convar). A static read suggested this happens at the next round start, but a diag log of a
  demo shows otherwise. The money, the win status, `m_totalRoundsPlayed` and the updated loss
  counters all change in the same frame at the round end. Only the CT dead enemies award comes
  later (about 430 ticks, just before the win status goes back to 0).
- Per dead enemy (award type 19, ~0x94ce90, at `cs_pre_restart`, game type classic with mode
  competitive / wingman / ...): always for the CT team (won or lost). The amount is
  `cash_team_per_dead_enemy` (50) times the number of dead Ts. Text:
  `#Team_Cash_Award_Dead_Terrorists:f` = " +$%s1 team income for {num_enemies} eliminated
  {num_enemies, plural, one{terrorist} other{terrorists}}" (ICU plural, resolved by us).
- Personal awards (~0xaf8920, `#Player_Cash_Award_*`) are always printed, also on the last round
  of a half. Only the team income in AddTeamAccount is skipped there.
- Round end reasons (TerminateRound notice table): 1 bombed, 7 defused, 8 CTs win,
  9 Ts win, 11 hostages rescued, 12 target saved, 13 hostages not rescued, ...
- Defaults:

  | Award | Amount |
  |---|---|
  | `cash_team_terrorist_win_bomb` | 3500 |
  | `cash_team_elimination_bomb_map` | 3250 |
  | `cash_team_win_by_time_running_out_bomb` | 3250 |
  | `cash_team_win_by_defusing_bomb` | 3250 |
  | `cash_team_win_by_hostage_rescue` | 3500 |
  | `cash_team_loser_bonus` | 1400 (+500 per consecutive loss) |
  | `cash_team_planted_bomb_but_defused` | 600 |
  | `cash_player_bomb_planted` / `_defused` | 300 |

What we do (the user's in-game experience: team income at the round end, the CT dead
enemies line at the next round start):

- Plant and defuse are personal awards queued on `bomb_planted` / `bomb_defused` by the
  target. They're printed with the money change as the fixed `cash_player_bomb_planted` /
  `_defused` (300), even when the money is capped. (A first version gave the whole change to
  the defuse and printed "+$550".)
- At the round end (win status 0 -> 2/3), the team income lines are printed right away. The
  amount is computed like AddTeamAccount does:
  - Winner: the `cash_team_*` value for `m_eRoundWinReason`.
  - Loser: `cash_team_loser_bonus + cash_team_loser_bonus_consecutive_rounds * min(losses,
    mp_consecutive_loss_max)`. The losses are `m_iNumConsecutive{CT,Terrorist}Loses - 1`: the
    client gets the already incremented count in the same frame as the round end. Teams start
    at 1, so losing the pistol round pays 1900.
  - Ts after a defuse also get `Planted_Bomb_But_Defused`. Ts alive when time ran out get
    `No_Income` instead.

  Convars are read with `g_pCVar->FindConVar` (the `mp_*` ones are replicated). Server
  defaults are used as fallback.
- Nothing is printed on the last round of a half or the match: `m_gamePhase` is 4/5, or
  `m_totalRoundsPlayed` (already counting this round) is `mp_maxrounds / 2` (with
  `mp_halftime`), `mp_maxrounds`, or the end of an overtime half (`mp_overtime_maxrounds / 2`).
- CT target: dead Ts are counted while the round is over, and the dead enemies line
  (`cash_team_per_dead_enemy` x count) is printed when the win status goes back to 0.
- The team money arrives later and is absorbed without a line: the printed amounts are
  subtracted from the following increases until a while after the next round start. Kills in
  the same change get the rest.

## Packaged as mirv_pov

The spike was packaged as `mirv_pov <idx> | 0 | list | info` (code: `AfxHookSource2/MirvPov.*`),
with the remaining switches under `mirv_pov_debug` and the IsHLTV spoof on by default. The
experimental `serverinfo` method (M2) was removed. Usage: `mirv_pov.md`. Commands named
`mirv_pov_spike ...` in this document are historical.

## Decision criteria at the end of the spike

- **Go for B** if either method gives a stable player view with most of the HUD native
  (crosshair, health/ammo, radar, flash) and no crashes on seek or round change. Remaining
  work is then fixes for whatever breaks, plus event-based reconstruction of
  server-private feedback.
- **Fall back to A (scoped swaps)** if the global swap crashes or makes the view unusable.
  The setter, array and killcam findings are still useful: a call-site-scoped swap can
  return `g_LocalPlayerControllers[0]`-style answers for selected callers only.

## Not yet explored

- The killcam flag byte `+0xcb` on `0x22362f8` (what the object is; whether it's needed).
- `0xc74ec0` / `0xb9c970` behaviour while the killcam override is active.
- Prediction: whether CS2 tries to predict the new local pawn in demo playback.
- `m_bIsHLTV` users in client.dll (spectator HUD decisions) for M1, where `is_hltv` stays
  true.
