# CS2 mirv_pov study

Study of what a CS2 "fake POV" needs, based on the CS:GO `mirv_pov` in this repo and the
community fork WangChuDi/advancedfx (commit `fc35fdad`, 2026-10-17; ~15k lines in
`AfxHookSource2/MirvPov*`, design log `doc/notes/cs2-pov-native-logic.md`).

**License rule for this work:** the fork's `mirv_pov` code is AGPL-3.0-only. Nothing in
these notes is copied from it; we only record how the game behaves and which design
choices they made. Any implementation here must be written from our own reverse
engineering, not transcribed from their source.

## 1. How CS:GO mirv_pov worked (this repo)

`AfxHookSource/csgo_net_chan.cpp`, `AfxHookSource/main.cpp` (FrameStageNotify, prediction wrapper).

- **Lie at the network layer:** rewrite `svc_ServerInfo` (`is_hltv=false`,
  `player_slot=target-1`) and `svc_SetView` (target entity) before the engine parses them.
- **Make the target the local player:** each `FRAME_NET_UPDATE_POSTDATAUPDATE_START` call
  `C_BasePlayer::SetAsLocalPlayer(target)` and flip `m_bIsLocalPlayer`.
- **Native HUD then works by itself.** The remaining hooks only repair the consequences of
  that lie: prediction off, eye angles from netvar, FOV, damage indicator filter,
  viewmodel sounds, rate cvars, disconnect fallback, ping-based interpolation compensation.
- About 12 hooks in total. dtugend: only feasible thanks to leaked CS:GO source (#875).

## 2. How the fork does it in CS2: "scoped presentation patching"

The real local player **stays a GOTV spectator**. The camera is CS2's native in-eye
spectator view (`spec_mode 1`), and the POV target is simply the current observer target
(`m_hObserverTarget` of the real local pawn, "auto-sync"). On top of that, about 20
presentation subsystems are fixed one by one.

### Core building blocks

| Technique | What it is | Where it's used |
|---|---|---|
| **Call-site-scoped local-pawn swap** | Detour the client's "get local pawn / controller" getters, but return the POV pawn **only** when `_ReturnAddress()` is in a whitelist of specific native call sites. A thread-local stack forwards the outermost return address through nested detours. | Team health bar, sound circles, death panel slide, death screen phases, buy menu, kill reward, flash |
| **Call-site-scoped observer-mode lie** | Detour an observer-mode helper and return "not observing" (0) only for listed callers. | Kill-streak HUD effect, flash predicate |
| **Inline byte patches** | Small code patches or trampolines at specific instructions (registers and stack contract recorded). | Radar team relationship and colors, TeamID, voice-ban, pickup prompt |
| **Calling native helpers to replay effects** | The demo lacks a player-private message, so they call the game's own helper with reconstructed inputs (for example `FindHudElement("CCSGO_HudDamageIndicator")` plus the damage-direction helper, or the SoundSystem trigger for deafen DSP and ringing). | Damage direction, deafen/tinnitus, radio audio, MVP music |
| **Game events as the data source** | Rebuild private feedback from public events: `flashbang_detonate` + `player_blind` (distance tiers 100/500/1000 → long/medium/short ring), `player_hurt` (HE `dmg_health >= 30` → deafen), `player_death` (headshot affects the death-cam phases). | Feedback, death cam, kill reward, round damage |
| **Panorama manipulation** | Add or remove CSS classes and inline style properties on HUD panels, with an "owned by POV" marker so they can be cleaned up. | Hide spectator panels, top-bar names, buy-zone icon, enemy health |
| **Schema by name** | All entity fields resolved through SchemaSystem by class/field name, never hard-coded. | Everywhere (about 50 fields) |
| **CVar management** | Save, set and restore things like `tv_listen_voice_indices`. | Voice routing, spectator cvars |

Shared plumbing: a central feature registry (about 36 toggles,
`mirv_pov_debug_feature <name> 0|1`), FrameStageNotify before/after callbacks
(`FRAME_RENDER_PASS`), game-event fan-out, seek detection via
`IDemoFile::GetDemoTick`, and full state reset on level init, seek, target switch and disable.

### What CS2 demos give us (and what they don't)

- **Available:** all entity state (controller/pawn, money/inventory services, `m_bIsBuyMenuOpen`,
  observer state, death time/headshot), game events, user messages that GOTV relays, voice
  data, and **recorded user-command buttons** (the fork's scoreboard replay reads the HLTV
  usercmd field). CS:GO GOTV didn't have that last one.
- **Missing or unreliable:** player-private user messages. For example, the `Damage` message
  is sometimes missing (they fall back to `player_hurt` with the attacker origin), the round
  `DamageList` is empty in GOTV (rebuilt from events), and server-side audio decisions
  (deafen) have to be re-derived. Where neither a message nor an event exists, the effect
  can't be faithfully reproduced.

### Feature inventory (fork) and the technique each needs

| Feature | Main technique | Notes |
|---|---|---|
| HUD cleanup (spectator panels, health/ammo) | Panorama + scoped swap | Must restore on disable |
| Flash overlay | Scoped predicate override (2 call sites) | Signatures break often |
| Radar (team colors, C4, smoke-visible teammates) | Inline patches + 2 detours | ~900 lines; 5 patch sites |
| TeamID / X-ray | Inline patches | Respects `spec_show_xray` |
| Team health bar | 5 detours, 35 return-address gates | Highest gate count |
| Sound circles | Getter detours with call-site whitelist | Two different getters in CS2 |
| Damage direction | Native helper replay | Message first, event fallback |
| Deafen/tinnitus | Event reconstruction + native audio | Thresholds from server.dll RE |
| Death screen phases / death panel | Scoped swap inside the native updater | Fixed a "black screen too early" bug |
| Kill reward / radio text / grenade voice | Message handler detours + localisation | Radio is ~3,900 lines (agent voice tables) |
| Voice routing | `tv_listen_voice_indices` masks + voice HUD | Team/all/enemy modes |
| Buy menu replay | 19 detours, **gated on exact client.dll SHA-256** | Hard-coded RVAs; breaks on every update |
| Scoreboard replay | Recorded usercmd buttons | Off by default |
| Pickup prompt, MVP music, round damage | Mixed | |

**Not addressed by the fork:** camera and view accuracy. There's no eye-angle,
interpolation or ping compensation, no FOV handling and no crosshair work. It relies
entirely on CS2's native in-eye spectating for the view itself. CS:GO `mirv_pov` did
have latency and interp compensation. This is a possible gap for analysis use.

## 3. Maintenance cost (the real problem)

- About **90 detours**, about **80 byte signatures** and about 15 inline patch sites, plus
  the shared files they extend (`DeathMsg.cpp` +850 lines, `RenderSystemDX11Hooks.cpp`
  +500, `MirvCommands.cpp` +350).
- Their log shows a re-adaptation after **every CS2 update**: 09-23, 09-26, 10-03, 10-09
  and 10-15 2026, roughly weekly. Each one needs IDA plus scripted signature, vtable-slot
  and struct-offset audits (for example the scoreboard slot offset changed from +0x34 to
  +0x38, and inventory or services offsets shifted).
- Many entries say "built, statically verified, **not run-tested**" (the work is heavily
  AI-agent assisted). Some fixes were later reverted after user reports. Expect to do
  runtime validation ourselves.

## 4. What this means for our approach

1. **Approach A (presentation patching) is proven but expensive.** Feature parity with the
   fork means owning a weekly-breaking surface of about 100 hooks. Feasible only if we limit
   scope to what matters for analysis.
2. **The fork is already halfway to approach B.** Its single most-used primitive is "return
   the POV pawn from the local-pawn getter at selected call sites". Approach B is the
   unscoped version of that: make the target the local controller/pawn everywhere (as
   CS:GO did with `SetAsLocalPlayer` / ServerInfo). The fork's growing per-call-site
   whitelists (35 gates in team health alone) show how much native code keys off "local
   pawn". If a global swap is stable, most of those features come for free.
3. **Risks for B in CS2:** prediction/sub-tick code running on a pawn without usercmds,
   input/camera ownership, Panorama assuming spectator, and server-only data still missing
   (B doesn't recover messages that GOTV never recorded). Event-based reconstruction (for
   deafen and damage direction) is still needed either way.

### Suggested plan

- **Step 1, spike B (time-boxed, about 1 week):** in demo playback, find the client's
  local-player controller slot (we already resolve client `GetSplitScreenPlayer(int)`,
  see `Hook_GetSplitScreenPlayer` in `ClientEntitySystem.cpp` / `main.cpp`) and the engine-side `CSVCMsg_ServerInfo` handling. Try a global
  local-controller swap to the observed player and record what crashes and what renders
  natively (HUD, radar, flash, crosshair).
- **Step 2, either way:** build the shared plumbing (target resolution from observer
  target, seek/level reset, feature registry, call-site-scoped getter swap helper). Both A
  and B need it, and a scoped getter swap is also the fallback when a global swap breaks
  one subsystem.
- **Step 3, prioritise analysis-critical features:** crosshair/scope/flash, radar, HUD
  health/ammo/money, damage direction, and view accuracy (interp/ping compensation as in
  CS:GO). Skip cosmetic replay (buy menu, MVP music, agent voices) at first. It accounts
  for the most code and the most update breakage.
