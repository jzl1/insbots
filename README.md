# InsBots

AI bot players for *Insurgency: Modern Infantry Combat* (Source 2006 / Episode 1 Engine, SRCDS build 4044).

---

## Description

**InsBots** is a standalone C++ Metamod:Source server plugin that adds full-featured, tactical AI bot players to *Insurgency: Modern Infantry Combat*.

### Key Features
* **Realistic Tactical Combat:** Threat perception (vision and hearing), stance transitions (standing, crouching, prone), cyclic fire control, recoil management, and tactical cover-seeking.
* **Objective-Driven AI:** Autonomous frontline push, territorial capture, dynamic sniper overwatch, and tactical smoke and grenade deployment.
* **Role-Valid Loadouts & Customization:** Randomized player skins, accessories, and class-specific weapon profiles.
* **Specialist Weapon Support:** M16A4/M203 40mm grenade launcher AI for grenadiers and RPG-7 rocket deployment.
* **Quota & Slot Management:** Dynamic bot quotas with automatic team balancing and reserved player slots for incoming humans.

---

## Installation Instructions

### 1. Windows Dedicated Server (`srcds.exe`)
* Copy `insbots.dll` and `insbots.vdf` into your server's `insurgency/addons/` directory.

### 2. Linux Dedicated Server (`srcds_run` / `srcds_i486`)
* Copy `insbots_i486.so` and `insbots.vdf` into your server's `insurgency/addons/` directory.

### 3. Navigation Meshes
* InsBots requires navigation meshes (`.nav` files) to navigate maps.
* Place the corresponding `.nav` files (e.g. `ins_sinjar.nav`, `ins_baghdad.nav`, `ins_almaden.nav`) into your server's `insurgency/maps/` directory.

### 4. Start or Restart your Server
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
| `insbot add [count]` | Add bots to automatically chosen team slots (default: 1). |
| `insbot add <slot>` | Add a bot to an exact team/class slot (e.g. `insbot add u2a3`). |
| `insbot kick` | Kick one bot from the game. |
| `insbot kickall` | Kick all bots from the game. |
| `insbot status` | Display bot roster, active nav mesh, and objective status. |
| `insbot list` | List all available settings, cvars, and current values. |
| `insbot get <cvar>` | Check the current value of a specific setting. |
| `insbot set <cvar> <value>` | Change any bot setting live during gameplay. |
| `insbot navreload` | Reload the navigation mesh for the active map. |
