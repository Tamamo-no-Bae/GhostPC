# Reaver on the Siege Tank base: snapshot of 2026-11-05

A copy of the Reaver vehicle's files as they stood before the jitter and collision fixes, kept in case we return to
this version.

What this snapshot has:
- **Base:** cTankControl (reaver_vehicle_player matches "arclite" in ControlNameMatch).
- **Driving:** velocity set every frame (0.9 m/s), turning by tThing::SetPhysMatrix.
- **Hull:** the Siege Tank's hull, scaled ×1.5.

Known problems in this version:
- It jittered while driving and while standing still.
- Its collision hull sat about 1 m ahead of the model.

Files and where each goes back:
- `vehicles.cpp`, `customsound.cpp`, `camera.cpp` go to `src/`.
- `make_ghostpc_templates.py` goes to `scripts/`.
- `make_vehicle_map.py` goes to `levels/vehicles/`.
- `ghostpc.nfx` goes to `game/Misc/`.
- `ghostpc.not` goes to `game/Misc/Templates/`.

To restore it, copy each file back to its folder and run `build.bat`.
