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
back by itself a few frames later. The demo UI's spectated player and kill/death markers
follow the POV player, also when seeking to a moment where they're dead.

## What works

- First-person view of the player with their native player HUD, killfeed and scoreboard (TAB).
- Player radar: teammates in their colours, enemies only when spotted, C4 carrier.
- The player's own crosshair (new-format `CS...` crosshair codes).
- No buy-menu blur when the player opens their buy menu.
- Death camera and the "killed you with ..." death card.
- Damage direction arcs when the player gets hit (pointing at the attacker).
- Round end damage report under the avatars (damage / hits given and taken, kill type). It's
  rebuilt from the demo's hurt and death events as they play. Seeking back into a round
  you've already watched is fine. If you skip part of a round by seeking forward, the report
  misses what you skipped.
- Chat, if the demo contains readable chat.
- Grenade radio lines of the player's team ("[CT] Name﹫Place: Flashbang!") and the player's
  kill reward lines (" +$300 for neutralizing an enemy with the AK-47"), rebuilt from the demo.
  Radio lines play the chat sound (not with `cl_clutch_mode 1`).
- The player's money lines: C4 plant / defuse rewards and the round end team income (win
  reason, loss bonus, planted-bomb bonus, CTs' $50 per eliminated terrorist, no income for Ts
  alive when time runs out). Like in the real game, team income shows right at the round end,
  the CT "eliminated terrorists" line at the next round start, and no team income lines on the
  last round of a half or the match.
- Only the player's own team's radio voice lines are heard (GOTV demos have both teams').
- Ear ringing (with the muffling) after flashbangs and big HE hits, and the C4 explosion screen
  shake, like the server does for players. Grenades don't shake the screen in CS2.

## Known limitations

- **AWP/SSG scope blur** doesn't follow the recording (local-player interpolation; parked).
- **Old-format crosshair codes** (`CSGO-...` version 1, common in pro demos) are rejected by
  current CS2, which then uses your own crosshair (same as normal spectating).
- **Chat in Valve matchmaking demos** is encrypted. Global chat only shows when watched via the
  in-game Watch UI, and team chat never does.
- **Damage direction arcs** point at the attacker, also for grenade and fire damage (the
  real game points at the grenade / fire).
- **Not recorded in GOTV demos, so not shown:** radio command lines ("Enemy spotted", ...).
- **Money lines:** kill rewards come from the player's money change, so there's no kill line
  when the money doesn't change (e.g. at max money). Team income amounts are computed from the
  round result and the server's money convars (the demo's replicated values, else CS2's
  defaults).

## Advanced switches (`mirv_pov_debug`)

| Switch | Default | Meaning |
|---|---|---|
| `hltv 0\|1\|2` | 1 | Tell the client it's not HLTV while in POV (needed for player HUD/radar) |
| `crosshair 0\|1` | 1 | Use the player's crosshair code |
| `buymenu 0\|1` | 0 | 1 keeps the recorded buy menu state (blurred view) |
| `predict 0\|1` | 0 | 1 lets the game predict the player's pawn (game default) |
| `deathcard 0\|1` | 1 | Show the death card |
| `damagearcs 0\|1` | 1 | Damage direction arcs |
| `radio 0\|1` | 1 | Grenade radio chat lines |
| `killreward 0\|1` | 1 | Money chat lines (kill, plant / defuse, team income) |
| `enemyradio 0\|1` | 0 | 1 keeps the enemy team's radio voice lines audible |
| `spectools 0\|1` | 0 | 1 allows spectator tools (x-ray) while in POV |
| `bombshake 0\|1` | 1 | C4 explosion screen shake |
| `earringing 0\|1` | 1 | Ear ringing after flashbangs and HE hits |
| `damagereport 0\|1\|show` | 1 | Round end damage report; `show` = now, with what's collected so far |
| `spectarget 0\|1` | 1 | Demo UI's spectated player = POV player (also while dead) |
| `chatlog 0\|1` | 0 | Log chat messages arriving at the chat HUD |
| `hltvlog [0\|1]` | 0 | Log which client code asks IsHLTV() |
| `diag 0\|1` | 0 | Diagnostic log to console and `%TEMP%\mirv_pov_log.txt` |

Technical background and findings: `mirv_pov_study.md`, `mirv_pov_spike.md`.

## Status (CS2 1.41.9.1, 2026-10-19)

Tested in game on a Valve MM demo and a pro tournament demo.

| Feature | State |
|---|---|
| POV swap, direct switching, `mirv_pov 0` | Working |
| Seeking (back and forward) | Working |
| Player HUD, radar, killfeed, scoreboard | Working |
| Player crosshair | Working for `CS-` codes; old `CSGO-` v1 codes fall back to yours |
| Buy menu blur suppressed | Working |
| Death camera, death card | Working |
| Damage direction arcs | Working; grenade / fire damage points at the thrower |
| Round end damage report | Working when played through; some inconsistencies after seeks |
| Demo UI spectated player / markers | Working; some inconsistencies after seeks |
| Chat | Working where readable (MM demo chat is encrypted) |
| Grenade radio and kill reward chat lines | Working (incl. chat sound) |
| Plant / defuse and team income chat lines | Working (verified against a money log) |
| Enemy radio voice muted | Working |
| No spectator x-ray in POV | Working |
| Flash / HE ear ringing, C4 screen shake | Working |
| AWP/SSG scope blur | Broken (parked) |

## TODO / next steps

Missing / to fix:

- [ ] **Seek inconsistencies.** The damage report and the demo UI spectated player sometimes
      differ after seeks (seen in game, not narrowed down yet). Reproduce with `diag 1` and
      `mirv_pov info`.
- [ ] **Forward seeks:** the damage report misses events in the skipped part of a round.
      Could fill it by reading the demo's events ahead, or by noting the gap.
- [ ] **Damage direction arcs, grenade / fire damage:** use the grenade or inferno position
      instead of the attacker's.
- [ ] **AWP/SSG scope blur:** the local player's `m_vClientScopeInaccuracy` interpolation
      ignores the recording (see `mirv_pov_spike.md`).
- [ ] **Old `CSGO-` v1 crosshair codes:** convert to the new format before applying.
- [ ] **View angles:** use the recorded usercmd angles for a smoother first-person view.

Polish / before upstreaming:

- [ ] Robustness pass: map change and new demo while in POV, warmup, bots, disconnects,
      players changing team, and every hook failing gracefully after a CS2 update.
- [ ] Decide which `mirv_pov_debug` switches stay. Drop the purely diagnostic ones
      (`hltvlog`, `chatlog`) or keep them hidden.
- [ ] Clean up `mirv_pov_spike.md` into a maintainer-facing notes file (hook list,
      signatures, what each one is for).
- [ ] Discuss with advancedfx maintainers (issue / draft PR). The CMake Build Tools fix is
      independent and could go as its own PR.
