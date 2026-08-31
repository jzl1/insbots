import math, random

# Simulation of ApplyCrowdSteering's queue decisions (0.19.6).
# All bots share one route through a single portal at x=PORTAL_X (a doorway),
# which is exactly the sinjar spawn-room scenario. The sim mirrors the plugin's
# decision math, not engine physics: it shows whether the queue logic freezes
# bots in open space before they reach the door.

DT = 0.1
MOVE = 400.0
PORTAL_X = 600.0


class Bot:
    def __init__(self, slot, x, y):
        self.slot = slot
        self.x = x
        self.y = y
        self.stopped = 0.0
        self.tp = 0


def _speed(pred_ahead, corridor, drop):
    if drop:
        if pred_ahead >= 260.0:
            return 0.85
        if pred_ahead > 180.0:
            return 0.85 * (pred_ahead - 180.0) / 80.0
        return 0.0
    if corridor:
        if pred_ahead >= 220.0:
            return 0.85
        if pred_ahead > 140.0:
            return 0.85 * (pred_ahead - 140.0) / 80.0
        return 0.0
    if pred_ahead >= 180.0:
        return 1.0
    if pred_ahead > 55.0:
        return 0.15 + 0.85 * (pred_ahead - 55.0) / 125.0
    return 0.0


def _near(bots, me):
    return any(
        o is not me and (o.x - me.x) ** 2 + (o.y - me.y) ** 2 < 600.0 ** 2
        for o in bots
    )


def decide_old(bots, me, corridor, drop, door):
    """Current 0.19.6 code: same-route window widened to 400u lateral, gap
    control still uses the lateral-projected 'ahead' distance."""
    if not _near(bots, me):
        return 1.0
    if not (corridor or drop or door):
        for o in bots:
            if o is me or o.slot >= me.slot:
                continue
            if (o.x - me.x) ** 2 + (o.y - me.y) ** 2 < 112.0 ** 2:
                return 0.65
        return 1.0

    pred = None
    pred_ahead = 1e9
    for o in bots:
        if o is me:
            continue
        rx = o.x - me.x
        ry = o.y - me.y
        if rx * rx + ry * ry > 750.0 ** 2:
            continue
        ahead = rx
        lateral = abs(ry)
        path_shared = True  # all sim bots share the route
        max_lateral = 400.0 if path_shared else (72.0 if (corridor or drop) else 52.0)
        max_ahead = 600.0 if path_shared else 300.0
        if ahead > max_ahead or lateral > max_lateral:
            continue
        if ahead <= 4.0:
            if ahead < -4.0 or o.slot > me.slot:
                continue
            ahead = 4.0
        if ahead < pred_ahead:
            pred = o
            pred_ahead = ahead
    if pred is None:
        return 1.0
    return _speed(pred_ahead, corridor, drop)


def decide_new(bots, me, corridor, drop, door):
    """Proposed fix: same-route ordering uses the along-route gap to the shared
    portal (pathGap). Abreast bots have pathGap ~0 and do NOT queue each other;
    only strung-out bots (pathGap >= 40) or a pair at the actual mouth yield."""
    if not _near(bots, me):
        return 1.0
    if not (corridor or drop or door):
        for o in bots:
            if o is me or o.slot >= me.slot:
                continue
            if (o.x - me.x) ** 2 + (o.y - me.y) ** 2 < 112.0 ** 2:
                return 0.65
        return 1.0

    pred = None
    pred_ahead = 1e9
    for o in bots:
        if o is me:
            continue
        rx = o.x - me.x
        ry = o.y - me.y
        if rx * rx + ry * ry > 750.0 ** 2:
            continue
        ahead = rx
        lateral = abs(ry)
        path_shared = True

        if path_shared:
            # Shared portal at (PORTAL_X, 0). Remaining distance to it decides
            # queue order, not the lateral projection.
            my_dist = math.hypot(me.x - PORTAL_X, me.y)
            o_dist = math.hypot(o.x - PORTAL_X, o.y)
            path_gap = my_dist - o_dist  # >0: other is closer to the portal
            near_mouth = my_dist <= 120.0
            strung_out = path_gap >= 40.0 and lateral <= 220.0
            mouth_pair = near_mouth and lateral <= 60.0
            if not (strung_out or mouth_pair):
                continue
            if path_gap <= 0.0 and not mouth_pair:
                continue
            metric = path_gap
            if mouth_pair and path_gap <= 4.0:
                if o.slot > me.slot:
                    continue
                metric = 4.0
        else:
            metric = ahead
            if metric > 300.0 or lateral > (72.0 if (corridor or drop) else 52.0):
                continue
            if metric <= 4.0:
                if metric < -4.0 or o.slot > me.slot:
                    continue
                metric = 4.0

        if metric < pred_ahead:
            pred = o
            pred_ahead = metric
    if pred is None:
        return 1.0
    return _speed(pred_ahead, corridor, drop)


def run(scenario, decide, corridor=False, drop=False, door=False, seconds=12.0, teleport=True):
    bots = scenario()
    timeline = []
    last_t = -1
    for step in range(int(seconds / DT)):
        for me in bots:
            sp = decide(bots, me, corridor, drop, door) * MOVE
            if sp < 1.0:
                me.stopped += DT
                if teleport and me.stopped >= 2.5:
                    me.x = PORTAL_X - 10.0
                    me.tp += 1
                    me.stopped = 0.0
            else:
                me.stopped = 0.0
                me.x += sp * DT
                if me.x > PORTAL_X - 10.0:
                    me.x = PORTAL_X - 10.0
        t = int(step * DT)
        if t > 0 and t != last_t and t % 4 == 0:
            xs = [b.x for b in bots]
            stopped = sum(1 for b in bots if b.stopped >= 1.0)
            last_t = t
            timeline.append(
                (t, round(min(xs)), round(sum(xs) / len(xs)), stopped, sum(b.tp for b in bots))
            )
    return timeline


def col(n=10, spacing=80):
    return [Bot(i, i * spacing, 0) for i in range(n)]


def sideline(n=5, spacing=120):
    return [Bot(i, 0, i * spacing) for i in range(n)]


def room(n=15, seed=7):
    rnd = random.Random(seed)
    return [Bot(i, rnd.uniform(0, 350), rnd.uniform(0, 800)) for i in range(n)]


def close_pair():
    return [Bot(0, 0, 0), Bot(1, 0, 120)]


def fmt(tl):
    return " | ".join("t=%ds min=%d avg=%d stop=%d tp=%d" % row for row in tl)


scenarios = [
    ("open column (no queue context)", col, dict()),
    ("open side-by-side (no queue context)", sideline, dict()),
    ("room -> door, corridor ahead", room, dict(corridor=True)),
    ("two abreast, corridor ahead", close_pair, dict(corridor=True)),
    ("column -> door, corridor ahead", col, dict(corridor=True)),
    ("room -> door, door-in-way (false-positive chokes)", room, dict(door=True)),
]

for name, scn, kwargs in scenarios:
    old_tl = run(scn, decide_old, **kwargs)
    new_tl = run(scn, decide_new, **kwargs)
    print("== %s ==" % name)
    print("OLD: " + fmt(old_tl))
    print("NEW: " + fmt(new_tl))
    print()
