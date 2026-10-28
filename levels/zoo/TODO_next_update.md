# Enemy Zoo / GhostPC: options for a future update

The 2026-10-25 list (Ultralisk arena, cloak shimmer, Hauler and Vulcan arenas) is done and shipped in the
2026-10-26 patch.

- **Cut Goliath boss (cAIBossGoliath, in progress after the 2026-10-26 build: BOSSGOLIATH on Goliath_222c):** a second Goliath boss, separate from the
  shipped TERRANGOLIATHBOSS (cAITerranGoliathBoss, Goliath_232). Its code is in Ghost.exe (aiClassBossGoliath.obj:
  boss bar in ScriptNotify, FireGoliathLaunchers / FireGoliathMachineGuns, run-MG / blowback / knockback controller
  modes named "BOSSGOLIATH :: ..."), but aiClass_Startup registers no AI name for it, nothing creates it and no
  template uses it: cut content in the state Tanagazj was in. Restoring it: register an AI name (e.g. BOSSGOLIATH)
  the way RegisterMissingAIClasses does for BOSSTANAGAZJ, check NewInstance/InitInstance for bugs, give it a
  template (a map-only override of a Goliath placement's ACTOR_AI) and an arena, then work out what its ScriptNotify
  messages (50, 300-302, the 100+ table) expect from a mission script.
- **BOSSARTEMIS:** Artemis's template asks for it but no such AI exists in the code; she stays a cutscene prop.
