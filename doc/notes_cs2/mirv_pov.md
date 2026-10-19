# mirv_pov (CS2)

Watch a CS2 demo from a player's own first-person view, as if you were that player: their
HUD, player radar (teammates, spotted enemies), crosshair, scoreboard and death card,
instead of the spectator versions.

**Demo playback only.** HLAE launches CS2 with `-insecure`. Never join VAC-secured servers
with HLAE.

## Usage

```
mirv_pov list        // players with their index
mirv_pov <index>     // watch as that player (you can switch directly to another index)
mirv_pov 0           // back to normal demo spectating
mirv_pov info        // status / diagnostics, also written to %TEMP%\mirv_pov_info.txt
```

Seeking (demo UI slider or `demo_gototick`) works: POV pauses during the seek and comes
back by itself a few frames later. The demo UI's kill/death markers follow the POV player.

## What works

- First-person view of the player with their native player HUD, killfeed and scoreboard (TAB).
- Player radar: teammates in their colours, enemies only when spotted, C4 carrier.
- The player's own crosshair (new-format `CS...` crosshair codes).
- No buy-menu blur when the player opens their buy menu.
- Death camera and the "killed you with ..." death card.
- Chat, if the demo contains readable chat.

## Known limitations

- **AWP/SSG scope blur** doesn't follow the recording (local-player interpolation; parked).
- **Old-format crosshair codes** (`CSGO-...` version 1, common in pro demos) are rejected by
  current CS2, which then uses your own crosshair (same as normal spectating).
- **Chat in Valve matchmaking demos** is encrypted. Global chat only shows when watched via the
  in-game Watch UI, and team chat never does.
- **Not recorded in GOTV demos, so not shown:** damage-direction arcs, flash/HE ringing, HE
  screen shake, radio texts and kill-reward ("+$300") chat lines, the round-end damage report.

## Advanced switches (`mirv_pov_debug`)

| Switch | Default | Meaning |
|---|---|---|
| `hltv 0\|1\|2` | 1 | Tell the client it's not HLTV while in POV (needed for player HUD/radar) |
| `crosshair 0\|1` | 1 | Use the player's crosshair code |
| `buymenu 0\|1` | 0 | 1 keeps the recorded buy menu state (blurred view) |
| `predict 0\|1` | 0 | 1 lets the game predict the player's pawn (game default) |
| `deathcard 0\|1` | 1 | Show the death card |
| `chatlog 0\|1` | 0 | Log chat messages arriving at the chat HUD |
| `hltvlog [0\|1]` | 0 | Log which client code asks IsHLTV() |
| `diag 0\|1` | 0 | Diagnostic log to console and `%TEMP%\mirv_pov_log.txt` |

Technical background and findings: `mirv_pov_study.md`, `mirv_pov_spike.md`.
