GhostPC - native Windows port loader for the StarCraft: Ghost Xbox prototype (May 2004 build)
============================================================================================

GhostPC.exe runs the prototype's own Ghost.exe (the Xbox executable, with Ghost.map) directly on
Windows: graphics on Direct3D 9, sound and music on XAudio2, input from keyboard + mouse or an
XInput controller. Nothing in the original build is modified on disk except Misc\ghostsp.nsc
(see below).

by Tamamo-no-Bae


INSTALL
-------
1. Copy the contents of this folder into the prototype's folder (the one containing Ghost.exe,
   Ghost.map, Levels, Sounds, Video ...). Allow the one overwrite (Misc\ghostsp.nsc).
2. Run GhostPC.exe.

Files in this patch:
  GhostPC.exe            the loader / port (new)
  GhostPC_README.txt     this file (new)
  Strings\game.nls       English text table; the build reads it from Strings\. A copy of
                         Misc\game.nls plus the lines for the Enemy Zoo test map and the Protoss
                         Arena (new)
  Misc\ghostsp.nsc       single-player mission list with the commented-out mission
                         1_3_REFINERY / 1_3_1a_Hive_StationOuter enabled and the Enemy Zoo test
                         map added at the end of the active missions (REPLACES the original)
  Misc\ghostmpcoop.nsc   the co-op mission list (missing from the build; made from ghostsp.nsc,
                         its own save slot) (new)
  Misc\ghostmpcomp.nsc   the competitive mission list: the Protoss Arena (new)
  Levels\8_1_1a_Protoss_Arena.nil / .nsd / .npd / .nlfx / .gpc
                         the Protoss Arena, the competitive map, and its level script (starts and
                         respawn points) (new level, see MULTIPLAYER)
  UI\Materials\811a_MenuImage.dds, 811a_MenuImageL.dds
                         its mission-select pictures (new)
  3D\Models\nova_ice.nod, 3D\Materials\NovaIce.dds, NovaIceFace.dds, Materials\ghostpc.nsa
                         Echo, player 2: Nova's model in the Ice skin (new)
  Levels\9_1_1a_Enemy_Zoo.nil / .nsd / .npd / .nlfx
                         the Enemy Zoo test map (new level, see below)
  Levels\9_1_1a_Enemy_Zoo.gpc
                         its GhostPC level script: start position, boss triggers (new)
  UI\Materials\911a_MenuImage.dds, 911a_MenuImageL.dds
                         its mission-select pictures (new)
  3D\Anims\zoo_mutaliskFlyAround.nad
                         the Enemy Zoo Mutalisk's flight path (new)
  3D\Motions\zoo_goliath_222c.nms
                         the Enemy Zoo's cut Goliath boss motion set (new)
  Sounds\custom\         added unit sounds, StarCraft's own (new, see CUSTOM SOUNDS)
  mapsrc\                the Enemy Zoo's source and build scripts, the Protoss Arena's in
                         mapsrc\arena (not needed to play, see their README.txt; built with the
                         OpenNOD SDK, github.com/aknavj/nodsdk-scghost)

Requirements: Windows 10 or 11 (uses the system's d3d9, XAudio2.9, XInput 9.1.0 and
d3dcompiler_47), any Direct3D 9 capable GPU.

Created at runtime next to GhostPC.exe: ghostpc.ini (settings and key bindings), ghostpc.log,
save\ (profiles and save games, the Xbox U:/T:/Z: drives).


SETTINGS WINDOW
---------------
Shown at startup (untick "Show at startup" to skip it; hold Shift while starting GhostPC.exe,
or run "GhostPC.exe -setup", to get it back). Options:
  - Resolution (4:3, 16:9 and 16:10 presets; the game is 4:3 and is pillarboxed on wide screens,
    rendering resolution scales with the output height)
  - Windowed / Fullscreen / Borderless fullscreen, VSync
  - Keyboard + mouse or Controller
  - Mouse sensitivity
  - Competitive time limit (3 to 30 minutes, 10 by default)


CONTROLS (keyboard + mouse, defaults)
-------------------------------------
  Xbox control             Key
  Left stick (move)        W A S D
  Right stick (look)       Mouse
  R trigger (fire)         Left mouse button
  Y (aim mode)             Right mouse button
  L trigger                Left Ctrl
  A                        Space
  B                        E
  X                        Q
  Black / White            C / X
  L thumb / R thumb        B / M (while aiming with Y: R thumb = sniper scope, also the middle
                           mouse button; in the scope L thumb = magnification, also the mouse wheel)
  D-pad                    Numpad 8 4 5 6 and the arrow keys
                           (D-pad up = thermal sight; D-pad down = flashlight, also F)
  Start (pause menu)       Enter or Esc
  Back                     Backspace
  -                        F10 releases / captures the mouse

Menus: arrow keys / numpad to move, Space to select. In Select Profile: Y creates a new profile,
X deletes the selected one (after a confirmation). Esc goes back: from profile select,
mission select, difficulty, options etc. one screen back; from "Select Profile / Options" to
the Press Start screen; Esc on the Press Start screen quits.

All bindings are in the [input] section of ghostpc.ini (several keys per button with "|",
e.g. "NUMPAD8 | UP"). The defaults follow the Cxbx-Reloaded profile "Cute1". An existing
ghostpc.ini keeps its own bindings: to get the newer F / middle mouse / wheel defaults, add them
("D Pad Down = NUMPAD5 | DOWN | F", "R Thumb = M | Click 2", "L Thumb = B | Wheel+ | Wheel-")
or delete the file to regenerate it.

With "Controller" selected, an XInput pad works like the original Xbox controller.


NOTES
-----
- The game runs at its original 30 fps.
- Mouse look bypasses the game's stick acceleration in all camera modes (normal, aim, sniper
  scope; the scope scales with zoom).
- Positional sound (distance falloff and left/right panning) follows the Xbox 3D audio
  calculation.
- It is a prototype: unfinished content, debug leftovers and prototype crashes are part of
  the build itself.


ENEMY ZOO TEST MAP
------------------
A new level (not part of the prototype), last in the mission list: an open-sky arena built with
the community OpenNOD SDK to look at the units up close. Rooms are sized for the largest units,
corridors and doors only fit Nova, so every area keeps its own units:
  Spawn room    Beacon (psi recharge), Rhynadon, masked Nova, health, ammunition and psi energy
  Protoss area  Nexus, Gateway, Pylon, Shield Battery, Photon Cannon, Crystal Guard, Reaver,
                Cloak Generator, Psi Dampener, Scout, Shuttle, Observer, Dragoon, Zealot,
                High Templar, Probe, Purifier, Dark Archon, Zangakar, Solara, Aura Kahn, Khandalor
  Terran area   Bunker, Missile Turret, Goliath, Siege Tank, Science Vessel, Vulture, Wraith,
                Dropship, Marine, Firebat, Trooper, Ghost, Scientist, SCV (psi), SCV driver,
                Mechanic, Spider Mine, Psi Dampener, Artemis, Hickson, Hauler, Nova's Vulture,
                elite Marines and captains, and a side room with the named Marine squad (Den,
                Joe, Jethro, Sacker, Chuggins, Lister, Dai and the Sidekick)
  Zerg area     Infested Command Center, Sunken Colony, Spore Colonies, Nydus Canal, Overlord,
                Mutalisk, Scourge, Zergling, Hydralisk, Infested Terran, Infested SCV, Larva,
                Egg, Pustule, Ultralisk (cutscene model)
  Boss arenas   Tanagazj, Vulcan, Hauler, Goliath, the cut Goliath boss, Aries (sniper), Lurker,
                Ultralisk; each has rifle and sniper ammunition, grenades, health and psi
                energy by its door
Notes: the Dark Templar has no model in this build. Boss AIs know where Nova is even through
walls; the other units only react when they see or are hit by her. The enemy Ghost cloaks
(use thermal sight). Levels\9_1_1a_Enemy_Zoo.gpc is read by GhostPC: Nova's start position and
the triggers that wake the bosses (the work of the missing mission scripts).


MULTIPLAYER
-----------
The prototype has a local two-player mode, split screen left and right, that its menus never
reached. GhostPC restores it: main menu -> Multiplayer. Player 1 plays on keyboard + mouse (or the
first controller), player 2, Echo (Nova in her Ice skin), on a controller. Each player picks a
profile (the second can't be the first's).

Cooperative: the campaign's missions for two. Each half has its own HUD, aiming, reticle and
calldowns (both players can call them down); the briefing, objectives and cutscene dialogue are
full screen. Either player can set off a mission's triggers, alone or together, and script moves
and cutscenes take Echo along (Nova is the one in the cutscenes). Either player's death ends the
mission ("Echo fell to her death."). Co-op has its own progress (its own save slot); a profile
named NOVA unlocks every mission.

Competitive: 2 Players (4 Players is for a later patch), Nova against Echo in the Protoss Arena, a
symmetric map with four pillars, side rooms, corridors and plenty of pickups. A dead player
revives on a button press at a random respawn point, away from her opponent. Kills count for the
shooter (grenades included; your own death doesn't count). The match timer
(top of the screen) runs for the time limit set in the settings window, pausing in the pause menu.
Hold Tab or Back (either controller, or Backspace) for the scoreboard. At the end the victory
screen names the winner (the profile names) for a minute, Space or A skips it, and the game
returns to the title screen. Start opens the pause menu (Quit) as in single player.


CUSTOM SOUNDS
-------------
Some units' sound banks are missing their attack sounds in this build. GhostPC plays WAV files
from Sounds\custom\<sound bank>\<cue>.wav in place of a bank cue (or where it is missing), with
~2, ~3 ... for random variants. Included: the Sunken Colony and the Lurker boss (StarCraft's
own sounds).


CHANGES (2026-10-29)
--------------------
- Multiplayer restored (see MULTIPLAYER): main menu Singleplayer / Multiplayer / Options.
  - Co-op: split-screen rendering and HUDs, player 2 (Echo) on her own controller with player 1
    on keyboard + mouse, starts beside player 1 in every level, per-player aiming, reticle and
    calldowns, triggers for either
    player, deaths end the mission, co-op profile menus with P1 / P2 titles and Delete, a
    scrolling mission list.
  - Competitive: the players menu, the Protoss Arena, random respawns, kills, the match timer
    and its option, the scoreboard, the victory screen.
- Typing a profile name: Backspace deletes, Shift for capitals, Esc goes back.
- Aries the sniper boss: 80 damage a shot (was 25).


CHANGES (2026-10-28)
--------------------
- Profiles: more than one profile in single player ("Create New" disappeared after the first),
  and profiles can be deleted (cut from the menus, restored: X on the profile list).
- Enemy Zoo: unlocked for every profile (the other missions still unlock through play); health
  and psi energy in every boss room, health, ammunition and psi energy in the spawn room.
- Fixed (every level):
  - Scout and Wraith: stay in their room, no longer sink to the floor, search where Nova was
    after a lockdown, fly back to their post 2 minutes after losing her; slightly wider spread.
  - Overlord: returns to its post after losing Nova (it flew off into a corner).
  - Lurker boss: its spikes hit Nova (75 damage; they passed through her), burrowed attacks
    do 90 (was 25).
  - Purifier: its beam aims at Nova and damages her (it went over her head).
  - Right and left mouse buttons also work when Windows' raw input misses a click.
- A crash report (ghostpc.log, ghostpc_crash.dmp) and checks against stale unit data after
  restarts.


CHANGES (2026-10-26)
--------------------
- Enemy Zoo:
  - Boss arenas: Aries the sniper boss (arena and triggers recreated for his cut AI), Goliath
    (new), the cut Goliath boss (new, see below), Lurker, Ultralisk (with Nova-sized loop
    corridors), Hauler and Vulcan (pillars for cover), Tanagazj (his own room). Ammunition and
    grenades in every boss room.
  - New units: Zergling, Hydralisk, Overlord, Larva, Egg, Infested SCV, Nydus Canal, Pustule,
    Spore and Sunken Colonies, Marine, Trooper, Scientist, SCV driver, Siege Tank, Wraith,
    Dropship, Spider Mine, Hickson, Hauler, Nova's Vulture, Cloak Generator, Psi Dampener,
    Beacon, Mechanic, elite Marines and captains, and the named Marine squad in a side room.
  - Props start in their stand pose and only react to Nova's shots.
  - Restarting after death returns Nova to the spawn room and re-arms the boss triggers.
- Fixed (every level):
  - Tanagazj: his cut boss AI is restored (warp blade, double damage, cloak as designed).
  - Probe turns to face where it goes.
  - Ultralisk boss reaches and hits Nova (tusks 100 damage, stomp 20 with a 5 s cooldown).
  - Sunken Colony: one spike per attack (it hit several times), the tentacle shows.
  - Overlord no longer dives into the floor and flies into walls after losing Nova.
  - Mutalisk flies its path and attacks from it (started by the level script).
  - Projectiles that stand still are no longer invisible.
  - Vulture: its AI was never hooked up (it stood still in every level); restored, and it now
    needs to see Nova instead of noticing her through walls.
  - Scout (and the Wraith, which uses its AI): hovers at range and shoots instead of flying into
    walls and up to the ceiling, aims at Nova, can be locked down (it exploded, once crashing the
    game), and its exhaust comes out of its four tail vents (it was one flame in its middle).
- Cut content restored: a second Goliath boss AI that no level used (runs between positions in
  its arena and fires rocket volleys; GhostPC stands in for its missing mission script). In the
  Enemy Zoo it drives Goliath_222c, with missing animations borrowed from the other Goliaths.
- Controls: the middle mouse button now reliably opens the sniper scope (quick clicks were lost),
  and the mouse wheel steps the scope's magnification (new default binding, see CONTROLS).
- The Enemy Zoo's source (mapsrc folder).
- Sounds for the Sunken Colony and the Lurker boss (see CUSTOM SOUNDS).
- Level scripts also load when the level is started from the mission menu.


CHANGES (2026-10-25)
--------------------
- Thermal sight works (the screen-blur shader reads the back buffer).
- Pixel shaders use per-stage combiner constants correctly (also changes how Protoss shields
  look; they now follow their shader's source).
- No more ground/shadow flicker during gunfire and no more texture fighting on walls: fixed
  function geometry is transformed exactly like the game's own shaders, and the Xbox W-buffer
  (linear depth) is emulated.
- 4x multisample anti-aliasing.
- New default bindings: F = flashlight, middle mouse button = sniper scope.
- Enemy Zoo test map.


CHANGES (2026-10-21)
--------------------
- First release.
- Fixed a crash in the movie decoder (privileged wbinvd instruction), reported by jason098.
