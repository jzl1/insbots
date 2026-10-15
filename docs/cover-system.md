# Cover System V2

Cover System V2 provides offline-baked and dynamically evaluated tactical cover points for AI bots, replacing basic nav-mesh hiding spots with directional occlusion masks.

---

## 1. 12-Sector Occlusion Model

Each cover point is evaluated across **12 angular sectors** (30° slices spanning 360°):
* `blockMask`: Directional occlusion measured at standing eye-height.
* `crouchMask`: Directional occlusion measured at crouching eye-height.

When evaluating an incoming threat vector, the angle is mapped to its matching sector. If the sector is masked as blocked, line of sight from the threat to the bot is occluded by geometry.

---

## 2. Cover Tiers & Low Cover

Cover points are categorized into tiers based on how many sectors offer obstacle protection:
* **Full Cover:** Occludes at least 7/8ths of surrounding sectors.
* **Partial Cover:** Occludes at least 4/8ths of surrounding sectors.
* **Spot Cover:** Occludes at least 2/8ths of surrounding sectors.
* **Low Cover (`isLowOnly`):** Obstacles (such as sandbags, low walls, or crates) that occlude only when the bot is crouching (`crouchMask`), but leave the bot exposed when standing (`blockMask`). Bots dynamically transition to a crouched stance upon occupying low cover.

---

## 3. Dynamic Selection & Ownership

* **Threat Scoring:** When engaged in combat, a bot scores candidate cover points within range by penalizing exposure toward the enemy sector, penalizing travel distance, and rewarding higher cover tiers.
* **Exclusive Reservation:** Bots claim ownership of their active cover point so teammates do not crowd into the exact same spot.
* **Peeking & Firing:** Bots duck behind cover to reload or break line-of-sight, standing or leaning out periodically to return fire.

---

## 4. Diagnostics & Administration

| Command | Description |
|---|---|
| `insbot coverdump [radius]` | Dumps cover points within radius (default: 300 units) of the host/player. |
| `insbot covereval` | Evaluates cover points against the player's current view and aim ray. |
| `insbot coverocc` | Displays all currently owned and active cover points occupied by bots. |
