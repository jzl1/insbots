# InsBots

AI bot players and vehicle combat systems for *Insurgency: Modern Infantry Combat* (Source 2006 / Episode 1 Engine, SRCDS build 4044).

## Quickstart

1. Download the latest release [here](https://github.com/jzl1/insbots/releases/latest)
2. Open the downloaded archive (`.zip` for Windows, `.tar.gz` for Linux).
3. Copy or drag the `addons` folder directly into `Source SDK Base/insurgency/` (merge folders when prompted).
4. Place navmeshes (`.nav` files) under Source SDK Base\insurgency\addons\insbots_nav (Grab them [here](https://github.com/jzl1/ins_navmeshes)) or build it by yourself with nav_generate.
5. Add these recommended settings to your `insurgency/cfg/server.cfg`:

```cfg
// Maintain 16 bots with 2 reserved slots for incoming human players
insbot set quota 16
insbot set reserve 2

// Randomized appearances and tactical weapon AI
insbot set randomize_customization 1
insbot set randomize_loadout 1
insbot set grenades 1     // opt-in: frag grenades are OFF by default
insbot set smoke 1
insbot set m203 1

// Humvee auto-turrets and bot anti-vehicle targeting
insbot set turret_enabled 1
insbot set turret_freeze_gate 1
insbot set target_vehicles 2
```

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
| `F3` / `!orders` | *(In-game)* Open the interactive squad orders radio menu. (Sourcemod required) |
| `F2` / `!request` | *(In-game)* Request a class slot currently occupied by an AI bot. (Sourcemod required) |

For detailed technical guides, advanced configurations, and the complete ConVar reference, see the [Documentation Index](docs/README.md).

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

