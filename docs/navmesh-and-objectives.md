# Navigation Meshes & Objectives

InsBots relies on navigation meshes (`.nav` files) to traverse map geometry and dynamically prioritizes game objectives based on game mode rules.

---

## 1. Navigation Mesh Setup

InsBots loads `.nav` files from the following paths (in order of priority):
1. `insurgency/addons/insbots_nav/<mapname>.nav`
2. `insurgency/maps/<mapname>.nav`

If no nav mesh is found, bots still move and fight — they fall back to direct
steering and roaming ("roam by feel") — but they cannot pathfind through map
geometry, so they get stuck on obstacles and wander instead of navigating.

### Reloading
To reload the mesh after authoring or updating without restarting the server:
```text
insbot navreload
```

---

## 2. Objective Intelligence & Game Modes

InsBots scans and maps all `ins_objective` brush entities on the level:
* **Home Bases & Spawn Zones:** Learns team spawn centers dynamically from bot spawn samples. Hidden spawn-zone objectives are automatically filtered out from assault targets.
* **Game Mode Recognition:** Automatically detects whether the map runs **Push**, **Firefight**, or **Battle**.
* **Push-Order Targeting (`insbot set push_order 1`):** On Push-mode maps, bots respect linear capture progression, focusing their assault only on the currently active frontline objective rather than scattering across locked rear points.

---

## 3. Pathing Recovery & Diagnostics

* **Learned Bad-Link Memory:** If bots repeatedly fail to traverse a broken or obstructed nav link, the pathfinder logs it as a bad link and routes around it. Use `insbot forget` to reset this learned memory if map geometry changes.
* **Forced Pathing (Goto):** For testing or debugging path traversal:
  - `insbot goto`: Forces bots to navigate to the exact point the host player is looking at.
  - `insbot goto <x> <y> <z>`: Orders bots to pathfind to specific world coordinates.
  - `insbot stop` / `insbot clear`: Releases the forced goto override and returns bots to normal AI.

---

## 4. Console Commands

| Command | Description |
|---|---|
| `insbot navreload` | Reloads the active map's navigation mesh. |
| `insbot nav [0\|1]` | Toggles the navigation pathfinding system on or off. |
| `insbot forget` | Wipes learned bad-link pathfinding memory for the active map. |
| `insbot objdebug` | Dumps current objective states, owners, capture progression, and team homes. |
| `insbot goto [x y z]` | Forces bots to crosshair hit location or given coordinates. |
| `insbot stop` / `insbot clear` | Releases active forced goto overrides. |
