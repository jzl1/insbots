# InsBots

AI bot players for *Insurgency: Modern Infantry Combat* (Source 2006 / Episode 1 Engine, SRCDS build 4044).

---

## Description

**InsBots** is a standalone C++ Valve Server Plugin (VSP) paired with an optional SourceMod companion plugin that brings full-featured, tactical AI bot players to *Insurgency: Modern Infantry Combat*. The core plugin loads directly from `addons/insbots.vdf` and does not require MetaMod:Source.

### Key Features
* **Realistic Tactical Combat:** Threat perception (vision and hearing), stance transitions (standing, crouching), cyclic fire control, and tactical cover-seeking.
* **Squad & Formation System:** Dynamic wedge formation escorting, velocity-oriented heading, collision avoidance (anti-boxing), and stance mimicry.
* **Orders & Suppression:** Interactive radio orders menu (F3 / `!orders`), covering-fire suppression with real-time target occlusion pathing, and dry-magazine reloads.
* **Objective-Driven AI:** Autonomous frontline push, territorial capture, dynamic sniper overwatch, and tactical smoke / grenade deployment.
* **Slot & Quota Management:** Dynamic bot quotas, reserved slots for incoming human players, and class slot requesting (`!request`).

---

## Installation Instructions

### 1. Windows Dedicated Server (`srcds.exe`)
* Copy `insbots.dll` and `insbots.vdf` into your server's `insurgency/addons/` directory.

### 2. Linux Dedicated Server (`srcds_run` / `srcds_i486`)
* Copy `insbots_i486.so` and `insbots.vdf` into your server's `insurgency/addons/` directory.

### 3. SourceMod Companion Plugin *(Optional but Recommended)*
* Copy `insbots_companion.smx` into your server's `insurgency/addons/sourcemod/plugins/` directory to enable squad commands, the F3 orders menu, and tactical world markers.

### 4. Navigation Meshes
* InsBots requires navigation meshes (`.nav` files) to navigate maps.
* Place the corresponding `.nav` files (e.g. `ins_sinjar.nav`, `ins_baghdad.nav`, `ins_almaden.nav`) into your server's `insurgency/maps/` directory.

### 5. Start or Restart your Server
* The Source engine automatically detects and loads the plugin on startup.
* Verify startup in the server console:
  ```
  [insbots] loaded
  ```

---

## Quick Start & Configuration (`server.cfg`)

To automatically manage and balance bots, add these settings to your `insurgency/cfg/server.cfg`:

```cfg
// Auto-maintain 16 bots (8v8 team balance)
insbot set quota 16

// Keep 2 free player slots open for incoming humans
insbot set reserve 2

// Enable randomized player skins, accessories, and role-valid weapon loadouts
insbot set randomize_customization 1
insbot set randomize_loadout 1

// Enable tactical grenade and smoke usage
insbot set grenades 1
insbot set smoke 1

// Enable M16A4/M203 40mm grenade launcher AI for grenadiers
insbot set m203 1
```

---

## In-Game Console Commands

You can execute these commands in the server console, RCON, or config files:

| Command | Description |
|---|---|
| `insbot add [count]` | Add bots to automatically chosen squad slots (default: 1). |
| `insbot add <slot>` | Add a bot to an exact squad slot (e.g. `insbot add u2a3`). |
| `insbot kick` | Kick one bot from the game. |
| `insbot kickall` | Kick all bots from the game. |
| `insbot status` | Display bot roster, active nav mesh, and objective status. |
| `insbot list` | List all available settings, cvars, and current values. |
| `insbot get <cvar>` | Check the current value of a specific setting. |
| `insbot set <cvar> <value>` | Change any bot setting live during gameplay. |
| `insbot navreload` | Reload the navigation mesh for the active map. |

---

## Squad & Radio Commands

Players can lead and direct AI squadmates using in-game chat or the SourceMod orders menu:

| Command / Key | Description |
|---|---|
| `F3` / `!orders` / `!squadorders` | Open the interactive squad orders menu (Move Out, Defend, Covering Fire, Backup, Dismiss). |
| `!squad` / `!lead` | Toggle bot fireteam escort mode (promotes player to squad leader). |
| `!dismiss` | Dismiss fireteam back to autonomous objective patrol. |
| `!mark` | Trace an eye-ray to mark a physical world surface for covering fire. |
| `!request` / `ins_request_slot` | Request a class slot currently occupied by an AI bot. |

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

