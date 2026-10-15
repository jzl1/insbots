# Squad Leadership & Radio Orders

InsBots allows human players to take command of AI bots as a fireteam leader through the optional SourceMod companion plugin (`insbots_companion.smx`) and the core VSP squad dispatcher.

---

## 1. Leading a Squad

Players can promote themselves to squad leader and request follower bots:
* **`F3` / `!orders`:** Opens the interactive SourceMod radio menu.
* **`!squad` / `!lead`:** Toggles squad escort mode, assigning nearby unassigned bots to follow the player.
* **`!dismiss`:** Releases follower bots back to autonomous territorial capture and defense.

---

## 2. Squad Orders & Behaviors

| Order ID | Name | In-Game Command | Description |
|---|---|---|---|
| `1` | **Follow (Move Out)** | Menu: *Move Out* | Followers fall into a dynamic wedge formation behind the leader. |
| `2` | **Hold** | Menu: *Defend Position* | Followers establish perimeter security and hold their current positions. |
| `3` | **Backup** | Menu: *Need Backup* | Calls unassigned team bots across the map to reinforce the leader's position. |
| `4` | **Suppress** | `!mark` / Menu: *Covering Fire* | Directs followers to lay down suppressive fire on a marked target surface. |
| `5` | **Advance & Defend** | Menu: *Advance & Defend* | Orders followers to advance to a marked waypoint and hold a triangular defensive perimeter there. |
| `0` | **Dismiss** | `!dismiss` | Returns followers to autonomous objective flow. |

---

## 3. Formations & Escort Mechanics

* **Dynamic Wedge Formation:** Followers hold staggered wedge offsets relative to the leader's movement velocity vector (rather than static world angles), keeping clear firing lanes.
* **Collision Avoidance (Anti-Boxing):** Followers monitor leader proximity and automatically side-step to avoid trapping the human player in doorways or narrow corridors.
* **Stance Mimicry:** Followers match the squad leader's stance (crouching or standing), dropping to low profile when the leader ducks behind obstacles.

---

## 4. Covering Fire Mechanics (`!mark`)

When a leader uses `!mark`, an eye-ray traces to the target surface in the game world:
* **Target Occlusion Repositioning:** If a follower's line of sight to the mark is blocked by walls or props, the bot maneuvers laterally along the nav mesh until a valid fire vector is established.
* **Cyclic Fire Pacing:** Followers fire sustained cyclic bursts at the designated point to suppress potential enemy peeks, automatically handling dry-magazine reloads.
* **HUD Waypoint Markers:** The companion plugin projects a visual world marker (native `UnitOrder` usermessage and 3D sprite) indicating the active waypoint.

---

## 5. Console Command Reference

Server admins can manually dispatch squad orders via the engine console:
```text
insbot squad <leader_client_id> <order_id> [wpX] [wpY] [wpZ]
```
* Example: `insbot squad 1 1` orders bot followers to follow client 1.
* Example: `insbot squad 1 4 1200 -450 64` orders followers of client 1 to suppress coordinates `(1200, -450, 64)`.
