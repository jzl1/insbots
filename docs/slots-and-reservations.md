# Slots, Quotas & Reservations

InsBots provides two distinct reservation mechanisms: server player slot buffering (to leave room for incoming humans) and squad role reservation (to prevent bots from occupying specific weapon classes).

---

## 1. Server Player Slot Reservation (`reserve`)

The `insbot set reserve` setting guarantees that a fixed number of player slots remain open for incoming human connections.

### Capacity Formula
The maximum number of bots allowed on the server at any moment is dynamically calculated:
$$\text{BotCap} = \text{maxClients} - \text{reserve} - \text{CountHumans()}$$

* **Human Joins:** As human players connect, `CountHumans()` rises, lowering `BotCap`.
* **Gradual Trimming:** If active bots exceed the new capacity, the plugin automatically kicks surplus bots at a paced rate (one bot every 0.25 seconds) until capacity is respected.
* **Configuration:**
  ```cfg
  insbot set quota   "16"  // Target bot population
  insbot set reserve "2"   // Always keep 2 slots open for humans
  ```

---

## 2. Squad Role Reservations (`insbot reserve`)

Squad role reservations lock specific class slots (e.g. Commander, Sniper, Marksman) so that AI bots cannot spawn into or take them.

### How It Works
* Up to 32 active reservations can be held simultaneously in the bot roster.
* Bots selecting loadouts during round setup (`JoinRoleId` and `PickPriorityClassRole`) check `IsRoleReserved()`. Reserved roles are skipped in both selection passes.
* Direct bot spawning via `AddBotToRole` rejects attempts to populate a reserved role.

### Failsafe Timeout
To prevent deadlocks if a player disconnects before claiming their reserved slot, `SweepReservations()` automatically releases any reservation inactive for longer than **300 seconds** (5 minutes).

---

## 3. Human Slot Takeovers (`!request`)

The SourceMod companion plugin (`insbots_companion.smx`) integrates directly with role reservations via the in-game `!request` command:
1. A player selects an occupied class role from the `!request` menu.
2. The companion kicks the bot currently holding that role.
3. The companion executes `insbot reserve <teamSlot> <slotCode> <userId>` to protect the empty seat.
4. Once the human player spawns into the class, the companion executes `insbot unreserve` to clear the reservation latch.

---

## 4. Console Commands

| Command | Description |
|---|---|
| `insbot reserve <teamSlot> <slotCode> [uid]` | Reserves a class slot (teamSlot: 0 or 1; slotCode: 5–34). |
| `insbot unreserve <teamSlot> <slotCode>` | Unreserves a specific team class slot. |
| `insbot unreserve_user <userId>` | Clears all reservations registered to a specific player ID. |
| `insbot unreserve_all` | Clears all active role reservations across both teams. |
