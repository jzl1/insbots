# Nav Fixups

Nav fixups are optional per-map sidecar files. They let a map correct known
malformed or unusually tight NAV transitions without adding map names, area
IDs, or coordinates to the C++ control flow.

## File location and name

Place the file next to the loaded NAV mesh and append `.fixups` to its full
path. For example:

```
addons/insbots_nav/ins_haditha.nav
addons/insbots_nav/ins_haditha.nav.fixups
```

The same file works for a map whose mesh is loaded from `maps/<map>.nav`.
If the sidecar is missing, the loader keeps the mesh and uses generic
pathfinding. If it is malformed, all fixups for that load are ignored and a
diagnostic is printed; the NAV mesh itself remains usable.

## Format

The first non-comment line must be the header:

```
# insbots nav fixups v1
```

Blank lines and `#` comments are allowed. Each data line is one of:

```
edge tight <fromAreaId> <toAreaId>
edge east <fromAreaId> <toAreaId>
transition <fromAreaId> <toAreaId> [takeoff <x> <y> <z>] [landing <x> <y> <z>]
```

`tight` marks approach edges that need the conservative doorway gate.
`east` marks the east-room choke edges. `transition` supplies optional
takeoff and landing hints for a separated drop; if both hints are present they
override the generic transition geometry. Area IDs must exist in the loaded
mesh. The parser is strict: an unknown keyword, invalid ID, incomplete hint,
or missing header discards the entire sidecar rather than applying a partial
set.

## Authoring and validation

1. Generate or regenerate the NAV mesh.
2. Confirm the area IDs with `pathdump` or the nav debug overlay.
3. Add only the edges and transitions that need correction.
4. Load the mesh in a native test or `bench_path` run and confirm the file is
   accepted.
5. Run `test_nav` and `grid_stress` for the map; a malformed sidecar must
   leave generic behavior intact.

The shipped examples are under `source/navfixups/`. Copy the matching file to
the game's `addons/insbots_nav/` directory with the `.nav.fixups` suffix.
