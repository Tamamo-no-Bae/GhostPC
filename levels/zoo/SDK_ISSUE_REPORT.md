# OpenNOD SDK 0.6.7: findings from building a new level from scratch

**Context:** I built a new, hand-written single-sector level (generated Quake III `.map`, about 33 brushes, 28 unit placements, 181 waypoints) and compiled it with `MAPtoNIL`, `MAPtoNSD` and `MAPtoNPD` from the 0.6.7 Windows release, using the same arguments as nodsdk-radiant's *Convert NOD level*. Testing used a PC port of the May 2004 Xbox build. All three compilers succeed after the workarounds below. The notes are in the order I hit them. Each item says whether it was **verified** in game or code, or is only **suspected**.

---

## 1. Archived (`.nsa`) materials are rejected as "Scripted sector shaders" (verified)

`MAPtoNIL: Scripted sector shaders are not supported yet` appears for any surface whose material is defined in an NSA archive. `nod_materials.c` sets `result.scripted = archived`, and `world_sector.cpp` rejects every scripted material.

- **The message is misleading:** nothing about these materials is scripted. They were plain `shader default` / `texture X` entries. I first assumed the `sky` shader was the culprit.
- **Consequences:**
  - Only direct DDS basenames that are *not* also material names in the shaderlist archives can be used.
  - Sky surfaces (`shader sky`) can't be authored at all.
  - Common textures can't be used if any global archive defines a material of the same name (`AR_Plated_blu_01`, `AR_Base_gry`, `GE_tracklgtSteel_blu`, `MS_Sky_Stormy_01`…).
- **Workaround:** compile the sky ceiling with a placeholder DDS, then rename its 32-byte entry in the NIL shader table to an archived sky material (`MS_Sky_Stormy_01`).
- **Suggestions:**
  - Say "archived NSA materials are not supported yet" in the error, and name the offending material.
  - Allow a pass-through for archived materials, which only need their name written to the shader table.

## 2. Waypoint/link/template entity keys are undocumented (verified)

Hand-written entities without the editor-generated keys fail with `Entity 54: Expected a MAP-only world-space waypoint/link, not imported NPD metadata`. That message gives no hint of the missing keys. Both MAPtoNSD and MAPtoNPD fail on the same entity.

- **Waypoints and links** need `nod_authoring "npd-world-v1"` and `nod_space "world"`, and links also need an `origin`.
- **Template placements** need `classname nod_template_instance` plus `nod_authoring "map-template-v1"`, `nod_template`, `nod_template_archive`, `nod_template_archives` and `nod_template_type`.
- I found these in `radiant/nodpathplacement.h` and `radiant/nodtemplates.h`; documenting them in the tools guide would help anyone generating MAPs programmatically.

## 3. The player spawns at the world origin, not at the `ghost` placement (verified)

The `ghost` placement compiles correctly: basedon, orientation, position (5, 59, 0.1), sector. Even so, the player is created at **(0, 0, 0)** and stays there.

- In single player, `gameMultiplayer_PlacePlayer` keeps the player's current position; only the multiplayer path uses `tWorld::FindPlayerStart`.
- Original levels appear to move the player by script or sequence: `ScriptThing::JumpToPlayerStart`, `tSequence::DoTeleToStart`.
- **Effect:** a scriptless MAP-built level starts the player wherever the origin happens to be. In my level the origin was the corner where two outer walls met the floor.
- **Workaround:** translate the whole map so the intended spawn point is at the origin.
- **Suggestion:** document this, or warn when the `ghost` placement is not at the origin.

## 4. First-frame physics NaN → `tPlayer::OutsideHavokWorldEvent` (verified, cause partly suspected)

Because of item 3, the player spawned embedded in that wall corner.

- **What happened:** from the first frame her position was NaN, Havok reported the body outside the world, and the game killed her through `OutsideHavokWorldEvent` → `tActor::Kill`. The death menu appeared after about three frames.
- **Verified:** replacing the SDK-generated collision body with a small body from an original level removed the NaN; the player then fell freely. So the NaN needs the generated collision.
- **Confirmed by follow-up:** after translating the map so the origin fell inside an empty room, the player spawned, landed on the generated floor and moved around normally. So the NaN comes from spawning embedded in the generated collision. The collision itself works for player dynamics once the spawn is clear.
- **Note:** your docs already say native queries were verified but that this *"do[es] not establish player/controller behavior, arbitrary dynamics"*. This is the first dynamics case I can report.

## 5. Physics bounds are fitted 1 m around the geometry (verified difference, impact unknown)

`MAPtoNIL` writes physics bounds of [min − 1, max + 1]. The original levels I checked keep 70–200 m of margin, and their origin is always well inside.

- I widened the bounds by 100 m during testing. This did *not* fix item 4 on its own, so it may be harmless.
- A larger default margin seems safer for anything that leaves the geometry, such as debris, ragdolls or flyers.

## 6. NSD trailer environment fields are all zero (verified difference, impact unknown)

The 192-byte trailer is written with:
- empty music and ambient names;
- zeroed environment fields: originals hold values such as flags and the distances 40, 50, 60, 20, 300, 600;
- empty sky-model and sky-animation names.

I copied an original trailer's music and environment fields; this did not fix item 4 either. It would still be good to expose these (music, ambient loop, sky model) as worldspawn keys, since without a sky model an open-sky level shows no sky.

## 7. Minor

- **Lighting triangle limit:** `nod_light_spacing` had to be raised to 20 for a roughly 120 × 135 m level. A face-count estimate of about 3100 still failed with "Lighting exceeds 4096 triangles". The final mesh was 3534 triangles. Printing the computed triangle count in that error would help in choosing the spacing.
- **Mapping layout vs docs:** `world_image.cpp` writes an offset word for every surface group, even when `mappingPresent` is 0. The docs (`formats/nil.md`) say the offset only follows a present mapping. It's harmless at the end of the file, but one of the two disagrees.
- **Missing archive warnings:** `NODscene: materials: missing editor archive 'Materials/organic.nsa'` (and `wood.nsa`) appears because `shaderlist.txt` lists archives this build doesn't ship. It's harmless but noisy.

Thanks for the SDK. Getting a brand-new level to load at all is impressive, and the source and docs made all of the above possible to track down.
