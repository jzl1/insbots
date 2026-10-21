# ConVar & Configuration Reference

All InsBots settings are configured in `server.cfg` or live in console using:
```text
insbot set <cvar> <value>
insbot get <cvar>
```

As of v0.21.1 the plugin exposes **261 settings**. This guide covers the
commonly tuned ones; the complete list is printed in-console by `insbot list`
and mirrored in `cvars.txt` at the repository root.

---

## 1. Population & Server Management

| Setting | Default | Description |
|---|---|---|
| `quota` | `0` | Target bot count. The plugin maintains this population automatically. |
| `reserve` | `2` | Number of player slots kept open for connecting humans. |
| `join_delay` | `5` | Seconds after server activation before quota bots begin joining (0 = immediate). |
| `quota_start_interval` | `0.20` | Seconds between initial quota client creations (minimum 0.05). |
| `quota_add_interval` | `0.25` | Seconds between steady-state replacement bot creations. |
| `randomize_customization` | `1` | Randomize skins, accessories, and player models. |
| `randomize_loadout` | `1` | Randomize role-valid weapons and gear. |

---

## 2. Combat & Perception

| Setting | Default | Description |
|---|---|---|
| `attack` | `1` | Master switch for bot shooting and attacks (0 = hold fire). |
| `detect` | `1` | Master enemy detection toggle (vision + hearing). |
| `fov` | `120` | Bot field-of-view cone in degrees. |
| `vis_range` | `2500` | Maximum enemy spotting distance in units. |
| `hear` | `1` | Bots hear enemy movement (footsteps) and investigate when not engaging. |
| `hear_range` | `1000` | Maximum units at which moving-enemy footsteps are audible. |
| `hear_speed` | `120` | Minimum horizontal speed (units/s) for movement to count as footsteps. |
| `aim_speed` | `260` | Maximum aim turn speed in degrees per second. |
| `aim_error` | `4.0` | Inherent aim variance in degrees. |
| `reaction` | `0.6` | Reaction delay before firing in seconds. |
| `lose_time` | `1.2` | Seconds a bot keeps tracking an enemy after losing sight. |
| `burst_pause` | `0.45` | Pause duration between weapon bursts in seconds. |
| `burst_time` | `0.30` | Firing burst length in seconds. |
| `freeze` | `0` | Freezes bot movement and actions for debugging (overridden by force pathfinding). |

---

## 3. Tactical Weapons, Grenades & Cover

| Setting | Default | Description |
|---|---|---|
| `grenades` | `0` | Frag grenade throws. **Opt-in** — set to `1` to enable. |
| `smoke` | `1` | Smoke grenade deployment to block sightlines. |
| `m203` | `1` | M16A4/M203 40mm grenade launcher AI for grenadiers. |
| `cover_v2` | `1` | Enables Cover System V2 12-sector evaluation. |
| `push_order` | `1` | Forces sequential objective targeting on Push-mode maps. |

---

## 4. Weapon-Class Combat Profiles

Combat behavior is tuned per weapon class. Negative values **inherit** the
corresponding global setting from section 2. `recoil_control` accepts
`inherit`, `realistic`, `perfect`, `aimerror`, or `percent` (with
`recoil_percent` 0-100).

| Class | detect_range | engage_range | stop_dist | reaction | aim_error | aim_speed | reserve_mags | recoil_percent |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `rifle_*` | -1 | -1 | -1 | -1 | -1 | -1 | 7 | -1 |
| `lmg_*` | -1 | -1 | -1 | -1 | -1 | -1 | 7 | -1 |
| `sniper_*` | 4000 | 4000 | 1600 | 0.5 | 1 | 210 | 25 | -1 |
| `shotgun_*` | 1400 | 850 | 300 | 0.5 | 6.0 | 320 | 50 | -1 |
| `pistol_*` | 2000 | 1300 | 450 | 0.3 | 4.5 | 300 | 5 | -1 |
| `rpg_*` | 3000 | 2400 | 900 | 0.2 | 0 | 360 | 10 | 100 |
| `grenadelauncher_*` | 3500 | 3500 | 1000 | 0.2 | 1.0 | 300 | 10 | 100 |

Every class also has a `*_recoil_control` setting (default `inherit`), and the
per-weapon safe-reload thresholds are dynamic:
`reload_safe_pct_default` plus `reload_safe_pct_<weapon>` (all `0.60`).

---

## 5. Humvee Auto-Turret & Vehicles

| Setting | Default | Description |
|---|---|---|
| `turret_enabled` | `1` | Master toggle for Humvee M249 auto-turrets. |
| `turret_aim_speed` | `90` | Turret traverse speed in degrees per second. |
| `turret_aim_error` | `4.0` | Aim inaccuracy in degrees (0 = perfect precision). |
| `turret_engage_range` | `1500` | Maximum engagement distance in units. |
| `turret_damage` | `50` | Damage per bullet (M249 cyclic fire rate: 800 RPM). |
| `turret_scan_speed` | `20` | Idle scan traverse speed in degrees per second. |
| `turret_acquire_fov` | `360` | Target acquisition cone (360 deg = omnidirectional). |
| `turret_spread` | `0.230` | Turret bullet spread cone in degrees (M249 real value). |
| `turret_fire_tolerance_yaw` | `5.0` | Fire gate: maximum yaw error in degrees. |
| `turret_fire_tolerance_pitch` | `4.0` | Fire gate: maximum pitch error in degrees. |
| `turret_freeze_gate` | `1` | Holds turrets dormant during pre-round freeze time. |
| `turret_freeze_fallback`| `6.0` | Fallback freeze duration when `mp_freezetime` is unreadable. |
| `target_vehicles` | `2` | Anti-vehicle policy: 0=ignore, 1=anti-armor only, 2=all weapons when no infantry visible. |

---

## 6. Voice & Squad Chatter

| Setting | Default | Description |
|---|---|---|
| `voice` | `1` | Master radio-callout toggle (0 = silent). |
| `voice_combat` | `1` | Combat voice callouts (engaging, reloading, enemy down). |
| `voice_objective` | `1` | Tactical objective chatter (securing, moving up, backup). |
| `voice_interval` | `4.0` | Minimum seconds between callouts from a single bot (anti-spam). |

---

## 7. Advanced Engine Offsets

These internal netprop and entity offsets are resolved per platform and should
not be modified unless diagnosing engine structural changes:

| Setting | Windows | Linux | Description |
|---|---:|---:|---|
| `team_offset` | `844` (0x34C) | `864` (0x360) | Objective ownership field. |
| `active_offset` | `948` (0x3B4) | `0` | "Is a real objective" flag; unused on Linux (validity comes from the verified parent/order/hidden fields). |
| `owner_offset` | `864` (0x360) | `864` (0x360) | `m_iCapturedTeam` (permanent capture, Push). |
| `capture_offset` | `960` (0x3C0) | `960` (0x3C0) | `m_iCaptureTeam`. |
| `order_offset` | `824` (0x338) | `844` (0x34C) | `m_iOrderID` within the active profile's ObjectiveData. |
| `hidden_offset` | `884` | `884` | Legacy. |
| `team_flip` | `0` | `0` | 1 = the owner field stores the enemy team (flip it). |
| `learn_persist` | `1` | `1` | Save/restore learned bad nav links (`addons/insbots_learn_<map>.txt`). |

---

## 8. Navigation & Pathfinding

| Setting | Default | Description |
|---|---|---|
| `avoid_jump` | `4000` | A* penalty for nav areas flagged JUMP (0 = don't avoid). |
| `avoid_crouch` | `2500` | A* penalty for nav areas flagged CROUCH (0 = don't avoid). |
| `avoid_marked` | `3000` | A* penalty for nav areas flagged AVOID (0 = don't avoid). |
| `avoid_ladder` | `3000` | A* penalty for ladder edges (0 = don't avoid). |
| `avoid_narrow` | `50` | Base penalty for narrow walk portals: x4 below 32 units, x2 for 32-55, x1 for 56-99; wider openings are free. Soft cost only, so a narrow route remains usable when no alternative exists. |
