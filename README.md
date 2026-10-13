# InsBots

AI bot players and vehicle combat systems for *Insurgency: Modern Infantry Combat* (Source 2006 / Episode 1 Engine, SRCDS build 4044).

---

### Highlights
* **Tactical Bot AI:** Threat perception (vision and hearing), dynamic Cover System V2 with 12-sector threat blocking, cyclic fire control, stance mimicry, and push-order objective targeting.
* **Vehicle Systems & Auto-Turret:** Autonomous roof-mounted M249 Humvee turret with hull-frame aiming and round-freeze gating; bot anti-vehicle combat and obstacle steering around hulls and wrecks.
* **Squad Orders & Companion:** Interactive radio orders menu (F3 / `!orders`), wedge formations, covering-fire suppression (`!mark`), 3D HUD waypoint markers, and class slot requesting (`!request`).

---

## Installation

1. **Windows Dedicated Server (`srcds.exe`):** Copy `insbots.dll` and `insbots.vdf` into `insurgency/addons/`.
2. **Linux Dedicated Server (`srcds_run`):** Copy `insbots_i486.so` and `insbots.vdf` into `insurgency/addons/`.
3. **SourceMod Companion (Optional):** Copy `insbots_companion.smx` into `insurgency/addons/sourcemod/plugins/` to enable squad commands, the F3 orders menu, and tactical world markers.
4. **Navigation Meshes:** Place corresponding `.nav` files (e.g. `ins_sinjar.nav`) into `insurgency/addons/insbots_nav/` or `insurgency/maps/`.
5. **Start or Restart Server:** The engine loads the plugin automatically on startup (`[insbots] loaded`).

---

## Quick Start (`server.cfg`)

Add these settings to your `insurgency/cfg/server.cfg`:

```cfg
// Maintain 16 bots with 2 reserved slots for incoming human players
insbot set quota 16
insbot set reserve 2

// Randomized appearances and tactical weapon AI
insbot set randomize_customization 1
insbot set randomize_loadout 1
insbot set grenades 1
insbot set smoke 1
insbot set m203 1

// Humvee auto-turrets and bot anti-vehicle targeting
insbot set turret_enabled 1
insbot set turret_freeze_gate 1
insbot set target_vehicles 2
```

---

## Essential Commands

| Command | Description |
|---|---|
| `insbot add [count]` | Add bots to automatically chosen squad slots. |
| `insbot kick` / `insbot kickall` | Kick one or all bots from the server. |
| `insbot status` | Display active bot roster, nav mesh status, and map objectives. |
| `insbot list` | List all configuration settings, cvars, and descriptions. |
| `insbot set <cvar> <value>` | Change any bot or turret setting live during gameplay. |
| `insbot turret status` | View registered vehicle turrets, team claims, and targets. |
| `insbot navreload` | Reload the navigation mesh for the active map. |
| `F3` / `!orders` | *(In-game)* Open the interactive squad orders radio menu. |
| `!request` | *(In-game)* Request a class slot currently occupied by an AI bot. |

For the complete ConVar reference, see [`cvars.txt`](cvars.txt).

---

## License

Released under the [GNU General Public License v3.0](LICENSE).

Copyright (c) 2026 jzl1

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, version 3 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program. If not, see <https://www.gnu.org/licenses/>.

### What this means in practice

You are free to run, study, modify and redistribute InsBots, including on your
own server and including as part of a paid server hosting service. If you
distribute a modified version, you must make its complete source code available
under the same license, and you may not add restrictions of your own on top.

