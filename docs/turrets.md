# Humvee Auto-Turrets & Vehicle Systems

InsBots includes autonomous server-side turret control for Humvees (`CPropVehicleDriveable`) and vehicle-aware bot combat/navigation logic.

---

## 1. Turret Architecture

The roof-mounted M249 operates autonomously without a player gunner:
* **Target Acquisition:** Scans in 360° (`turret_acquire_fov`) at `turret_scan_speed` deg/s, tracking visible enemies within `turret_engage_range` units.
* **Aiming Math:** Yaw and pitch demands are solved in the vehicle hull's local coordinate frame, respecting physical elevation and depression constraints.
* **Ballistics & Fire Control:** Fires automatic cyclic bursts (800 RPM) with authentic M249 gunshot sounds, red bullet tracers, and configurable `turret_damage` (default 50 per bullet) and `turret_aim_error` (default 4.0°).

---

## 2. Team Allegiance & Spawns

A Humvee turret's allegiance is tracked in the plugin's internal turret registry:
* **Neutral Spawn (First-Boarder Claim):** Spawned unassigned; the turret permanently adopts the team of the first human or bot player who sits in the driver seat.
* **Pre-Registered Spawns:** Map presets configured in `addons/sourcemod/configs/insvehicles/<map>.cfg` are pre-assigned to team 1 or team 2 at round reset.
* **Round-Freeze Gate:** When `turret_freeze_gate 1` is enabled, turrets remain dormant during pre-round freeze time so pre-registered vehicles cannot attack players trapped in spawn rooms.

---

## 3. Bot Anti-Vehicle Combat & Steering

Bots perceive hostile Humvees through a dedicated vehicle threat channel (`target_vehicles`):
* `0`: Bots ignore hostile vehicles.
* `1`: Only bots equipped with anti-armor weaponry (RPG-7, M203) engage hostile Humvees.
* `2` (default): All bots attack hostile Humvees when no infantry threat is visible (small arms deal scaled hull damage; RPG/M203 deal full damage).
* **Steering & Obstacle Routing:** Bots dynamically detect driveable vehicle hulls and burning wrecks, pathing smoothly around them without spinning or becoming stuck.

---

## 4. Administration Commands

| Command | Description |
|---|---|
| `insbot turret status` | Displays all registered turrets, entindices, teams, targets, and freeze state. |
| `insbot turret add <ent> [team]` | Manually registers an unmanaged vehicle entity. |
| `insbot turret register <ent> <team>` | Pre-registers a vehicle to a specific play team (1 or 2). |
| `insbot turret remove <ent>` | Removes and disarms a vehicle turret from the registry. |
| `insbot turret set <all\|ent> <cvar> <val>` | Overrides a setting globally or for a specific vehicle. |

---

## 5. Configuration Reference

```cfg
insbot set turret_enabled              "1"     // Master toggle for auto-turrets (1=on, 0=off)
insbot set turret_aim_speed            "90"    // Traverse speed in degrees per second
insbot set turret_aim_error            "4.0"   // Aim inaccuracy in degrees (0=perfect)
insbot set turret_engage_range         "1500"  // Maximum firing range in units
insbot set turret_damage               "50"    // Damage per bullet (M249 cyclic)
insbot set turret_scan_speed           "20"    // Idle scan traverse speed (deg/s)
insbot set turret_acquire_fov          "360"   // Acquisition cone (360=omnidirectional)
insbot set turret_freeze_gate          "1"     // Hold turrets dormant during freeze time
insbot set target_vehicles             "2"     // Bot anti-vehicle policy (0=off, 1=anti-armor, 2=all)
```
