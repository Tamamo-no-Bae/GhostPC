# StarCraft: Ghost → PC (GhostPC)

## What's in C:\temp\StarCraft Ghost (original dump, untouched)
- `Star.exe`, `Star_d.exe` (+`Star_d.pdb`), `Star_u.exe`: real **Win32** builds (VC6, Oct/Aug 2002, the Tokyo Game Show
  demo era, NOD engine). They use **Havok 1.x**; the data in the dump is the 2004 build (Havok 2, new formats), so they
  can't load it (Star_d asserts `numLocStrings < 1024` on the new string tables right away). Dead end for a playable port.
- `Ghost.exe` (+`Ghost.pdb`, `Ghost.map`), `GhostR.exe`, `GhostU.exe` (+map): **not 16-bit** — they are Xbox PE images
  (subsystem 14) produced before imagebld. `Ghost.exe` = May 8 2004, XDK 5659, full symbols, has `.reloc`.
  `Ghost.xbe` is the same build. `default.xbe` / `GhostR.xbe` / `GhostU.xbe` are other builds.
- `VertexShaders\*.xvu` = PC vs.1.1 token versions of shaders, `VertexShaders\XBox\*.xvu` = NV2A microcode the game
  loads, `*.ps` = pixel shader sources. Useful to verify the microcode translator (scripts/vscheck.py).
- String tables are in `Misc\*.nls`, but the build reads `d:\Strings\` → copy them to `Strings\` (done in game\).

## Approach: GhostPC (native host for the Xbox Ghost.exe)
`GhostPC.exe` (src\, build.bat → build\, copied to game\) maps Ghost.exe into a Win32 process and replaces the XDK
layer by symbol name (from Ghost.map). Game code, Havok, CRT, Bink, D3DX, XGraphics run unmodified.
- loader.cpp: maps image (relocates if 0x400000 busy), symbols, xboxkrnl imports, hooks (JMP; int3+VEH for <5-byte
  funcs), every other function in d3d8/dsound/xacteng/xapilib/xnet is a **trap** that logs its name and exits.
  Skips ring-0 instructions (`wbinvd` in the DivX decoder). Runs C/C++ initializers itself (skips XDK ones).
- kernel.cpp: crit sections (Xbox CS is 28 bytes → pointer to a Win32 CS), LIBCMT per-thread data via Win32 TLS.
- xapi.cpp: files (D:\ = game dir, T:/U:/Z: = game\save\X), threads, **16-byte aligned heap** (game uses movaps),
  find handles closed via CloseHandle, save games, launch info (command line).
- input.cpp: XInput pads + keyboard. net.cpp: Winsock passthrough but `socket()` fails (avoids a firewall prompt).
- audio.cpp: DirectSound streams (XMediaObject vtable) and buffers on XAudio2, Xbox ADPCM decoder. xact.cpp: **stub**.
- d3d_*.cpp: Xbox D3D on D3D9. Resources keep Xbox memory layout (swizzle, formats) and upload when dirty.
  Vertices converted per draw. Push buffers = game-built index lists (BEGIN_END/ARRAY_ELEMENT16).
  VS: NV2A microcode → HLSL (vs_2_a). PS: combiners → HLSL (ps_2_b); the fixed-function texture stages are also
  compiled to HLSL with Xbox semantics (CURRENT starts as diffuse; D3D9 FF got it wrong).
  Internal resolution x2 (env GHOSTPC_SCALE).

## Hard-won facts (XDK 5659 / this build)
- Render-state indices verified from the setters (scripts/rsidx.py): complex states 136..165 (PSTEXTUREMODES=136),
  deferred block from FOGENABLE=92 (WRAP0=98, LIGHTING=102), texture stages 32 dwords. PS states 0..55 mirror
  D3DPIXELSHADERDEF; SetPixelShader copies the def into D3D__RenderState and constants are read from there.
- Simple render states arrive as NV2A methods (`@D3DDevice_SetRenderState_Simple@8`, values are GL enums).
- `@D3DDevice_SetVertexShaderConstant1/4/NotInline` take the **hardware** register (API+96, added by the inlined
  wrapper) and NotInline's count is in **DWORDs**.
- NV2A VS: `oPos` aliases `r12`; the XDK appends a viewport trailer reading c[-38]/c[-37] (hw 58/59) → dropped,
  final position = r12 (clip space). Shaders without the trailer are screen space.
- NV2A VS: when MAC and ILU are both active, the ILU temp result always goes to **R1** (even if MAC writes only an
  output). Getting this wrong darkened all skinned lighting.
- Xbox fullscreen video uses the overlay (UpdateOverlay with a YUY2 surface) → drawn as a quad at Swap.
- Quad lists: n/4*2 triangles.
- SetPixelShader copies 57 dwords of the def into D3D__RenderState (PSTEXTUREMODES also at 54, DOTMAPPING 55,
  INPUTTEXTURE 56); the PS handle object reaches its definition through a pointer at +8.
- Bink video textures (portraits, Database): cVideoTexture locks once and Bink keeps decoding into that memory, so
  locked textures are "watched" (hashed once per frame when used) and re-uploaded on change.
- Input: Cxbx-style bindings in game/ghostpc.ini (default = Cxbx profile "Cute1" + arrows on the D-pad), mouse via
  raw input -> right stick (MouseAxisRange counts/frame = full deflection). GHOSTPC_INPUT="frame:Control,..." scripts
  presses for tests.
- Fog: with a vertex shader, NV2A applies the fog table (linear/exp/exp2, FOGSTART/END/DENSITY) to the shader's
  oFog value (the game writes a plane distance), not to depth. D3D9 table fog used depth and fully fogged ground,
  legs and hair (flat gray/purple). Now the translated VS computes the factor (ex[7]) and D3D9 uses it as vertex fog.

## Debug tooling
- env: `GHOSTPC_SHOT=900,1200` (save frames to test\frameN.bmp), `GHOSTPC_TRACE=900` (log every draw of a frame),
  `GHOSTPC_DUMPVS=1` (dump VS/PS/FF HLSL + microcode to test\), `GHOSTPC_TINT=a-b` / `GHOSTPC_TINTMODE=ps|vsff|ff`
  (paint draws magenta), `GHOSTPC_WHITE=N` (white diffuse for N-instruction shaders), `GHOSTPC_CAPVS=N`
  (capture constants + vertices → test\vsstate.bin), `-v` verbose log.
- scripts: gdis.py (symbolic disassembler), callgraph.py/hookset.py (what must be replaced), ucdis.py (microcode
  disassembler), vscheck.py (microcode vs PC shader on captured state), hlslrun.py (runs generated HLSL),
  tools\vstest.exe (runs a generated VS on the GPU), scripts\runtest.ps1 (timed self-closing test run).

## Status (2026-10-21)
- Boots, plays intro videos (picture + sound), title screen renders matching an Xbox reference at 60 fps.
- Since then: keyboard/mouse, gameplay reachable (first level, HUD, pause menu, briefings with Bink portraits).
- Audio: the original XACT engine runs unmodified on our Xbox DirectSound (audio.cpp, XAudio2). Needed: kernel
  timer/DPC thread + IRQL emulation (per-thread IRQL in TEB.ArbitraryUserPointer, fs:[0x24] reads patched to fs:[0x14],
  global lock at DISPATCH), APU sample counter read at 0xFE80200C patched to a call, CHRTFSource vtable filled via
  SetFullHRTF5Channel, pure dsound math (3D calculator, pitch) passed through (src/passthrough_gen.h).
  XWB v3: entries packed at 4-byte alignment -> decode ADPCM from the play region start (block alignment is per entry).
  Format DWORD: tag bits 0-1 (0 PCM, 1 Xbox ADPCM), channels bits 2-4, rate bits 5-30, bit 31 16-bit.
- The build presents at D3DPRESENT_INTERVAL_TWO (30 Hz). The movie players advance per Swap, so 60 Hz doubled movie
  speed and they skipped audio to keep up (the "scratchy" cinematics). Swap is paced on a 2/59.94 s schedule.
- XACT music uses WMA streams: the Xbox WMA decoder objects in dsound.lib run unmodified (kept list in loader.cpp).
- Streams without SetPitch/SetFrequency play at their format's own rate (44.1 kHz movie audio ran 9% fast before).
- Log lines are timestamped; the previous session's log is kept as ghostpc.prev.log.
- Front end (shell.cpp): cShell at shell_Shell; +0x3C state, +0x6EE0 current menu id (LoadMenu jump table: 1-12 in-game,
  13 MakeProfile, 14 Main "Select Profile/Options", 15 Start "Press Start", 16 Attract, 17 MissionSelect, ...),
  +0x6EFC menu shown. Esc: Main -> RequestMenu(15); Start -> quit; in missions it presses Start (Enter still works).
- Settings window (settings.cpp) before the game: resolution, windowed/fullscreen/borderless, VSync, keyboard+mouse or
  controller, mouse sensitivity, "show at startup" (Shift or -setup brings it back); saved in ghostpc.ini [video]/[input]/
  [launcher]. Rendering = 640x480 x RenderScaleFor(output height), presented 4:3-fitted (D3DSWAPEFFECT_COPY + dest rect).
  Test runs set GHOSTPC_NOSETUP=1 (scripts/runtest.ps1).
- Not done:
  options (resolution/widescreen/fullscreen), packaging.

## Mouse look (src/camera.cpp)
- Third person / behind / side etc.: camera turn = cControl::GetCameraYaw/GetCameraPitch (right stick, 0.1 deadzone,
  squared) x dt x 160/80 deg/s. Hooked: controller part reimplemented, mouse added as exact degrees / (dt*rate).
- Aim (Z-target; GunCrouch/GaussRifle states): ZTargetMovement -> GetCameraAxes (same shaping + ProjectCube); target
  velocity 200/100 deg/s (x0.3/x0.5 below 80% deflection), accelerated, applied to tSimCamera +0x20C yaw / +0x204 pitch
  (pitch -47..50). Mouse is added straight to those angles in the GetCameraAxes hook.
- Sniper: UpdateCameraSniper reads the pad inline; zoom factor 0.15+0.65*(fov-5)/70, fov at camera+0x68 (5..75),
  90/50 deg/s, pitch +-90. Hooked with a naked stub that adds the mouse (scaled by tan(fov/2)/tan(37.5)), replays
  the 5-byte "mov eax,[stdTime+0x5C]" prologue and jumps to +5 (CameraInit checks the prologue before ApplyHooks).
- Mouse no longer feeds the right stick while captured in KBM mode (MouseLookActive); 0.15 deg/count at 100%.
- Invert options: player info (ctl+0x180 -> +0x520 -> +0x70) +0x248 bit 4 = X, bit 2 = Y.

## 3D sound attenuation
- XACT (unmodified) runs the Xbox 3D calculator and passes DS3DCALCVOICEDATA to IDirectSoundBuffer/Stream_Set3DVoiceData.
  dwChangeMask (1 dist, 2 cone, 4 front/rear, 8 center/LR, 0x10 I3DL2, 0x20 doppler, 0x40 azimuth) only flags values
  GetVoiceData changed in XACT's per-source struct; pooled voices reassigned to a source never saw the bits, so they
  played at full volume. Apply3D now takes distance/cone/doppler/azimuth unconditionally.
- flFIRFilterAzimuth is in degrees (0 ahead, + right, +-180 behind), not radians.
- In-level values check out: rolloff 1, min distance 1 (game units), distances 6..113 -> 0..-41 dB.

## Esc in front-end menus (shell.cpp)
- eShellMenu (PDB): 0x11 MISSION_SELECT, 0x12 DIFFICULTY, 0x13 SELECT_PROFILE_SINGLEPLAYER, 0x17 SELECT_PROFILE_OPTIONS,
  0x18-0x1B options/settings, 0x1E LOAD_SAVEGAME, 0x22 CREDITS. Esc in 0x11..0x22 presses B (the menus' Back).

## wbinvd in M4Decoder::decode (reported by jason098)
- f5vid's M4Decoder::decode (cutscene video) runs "wbinvd" (0F 09, +0xE8 = RVA 0x2218C8) per frame when decoder+8 == 0.
  VehHandler always skipped it (logged at that address during the intro), but a debugger breaks on the first-chance
  EXCEPTION_PRIV_INSTRUCTION before the VEH runs, which looks like a crash. PatchPrivileged now NOPs it at load.

## Unused content research (Protoss, Psi Blade)
Tools: scripts/notdump.py (Global.not templates, modifier names from re/modifiers.txt = gameTemplate_aModifiers),
scripts/nsdthings.py (placed things in Levels/*.nsd).
- Data formats: Misc/Templates/Global.not = index (name[32]+u32 off) of templates; record = u32 count + count x
  (u32 modifier id, 32-byte value). Level .nsd placed things use the same modifier lists (id 0 basedon = template,
  1 orientation, 2 position, 3 sector, 15 script, ...). Per-level precache = 3D/Models/<lvl>.nmb, 3D/Anims/<lvl>.nnb,
  Materials/<lvl>.nsa + .xpr (built from -reslog / -lvlreslog runs).
- Misses fall back: renderModel_Load and tRenderKeyTrack::LoadData load loose .nod/.nad; all material archives in
  Materials/shaderlist.txt (incl. 3dmaterials) load at startup (worldEffects_Startup); terran/zerg/protoss .xwb are
  in aPreloadBundles; effects in Misc/game.nfx are global.
- Zealot: cAIProtossZealot ~10 KB full AI; template "zealot" (+ zealot_hib, zealot_prop) complete: zealot.nod,
  zealot_collide.nod, zealot.nms (50 anims, all present), ptssZealot foley, 120 HP + 80 shield, AI PROTOSSZEALOT.
  Its model/anims are only in the Siege/Goliath/Revealed banks (levels not in the dump).
- Sabotage (1_3_1b): intro 131b_ZerglingIntro is camera-only (cams ~(-6,8)), films corpse scvdriver_dead (2.4,5.1)
  and the cAIPause zerglings at nsd 0x3795 (3.5,3.8) and 0x3881 (1.4,3.8). ShuttleEscape casts zergling_cut actors.
  Nearest other zergling: 0x396d (17.8,-8.1) cAIChargePlayer. Template name field = 32 bytes at the listed offset.
- Player melee (user: a punch, not a blade) = button index 1 (B) at cPlayerControl+0x1EC (MapControls): Walk/Sneak/GunCrouch/
  GaussRifle -> MeleeAttack -> state 0x1E Melee -> MeleeStart -> ChangeToBlade (PLAYERWEAP_MELEE). Also used by the
  silent takedown. "PSIBLADE 0|1" in the .nsc is parsed into cMission+0x10C bit 0 but nothing reads it.
  MeleeCombo has no references (unused combo).
- Flamethrower: PLAYERWEAP_FLAMETHROWER, fired from GaussRifleUpdate/GunCrouchUpdate, but no Nova model and
  "flamethrower" is not an INVENTORY token (valid: canister smokegrenade stickygrenade spidermine hellfiregreande
  firefly mantis).
- DONE (local copy game\): Sabotage nsd 0x396d "zerglingnormal" -> "zealot" (12 bytes). Original in re\backup\.
  Loads without any precache changes; full model/materials, psi blades, charges and kills Nova (user confirmed).
  Quick test: GhostPC.exe -level 1_3_1b_Hive_StationOuter (scripts\runtest.ps1 -gameArgs).
- Also swapped (user confirmed working): 0x3cd1 (8.3,-37.4) -> hightemplar, 0x58c1e (23.6,-39.4) -> purifier ("Vindicator"
  is not a name in this build; Purifier assumed). Both plain zerglings (no script/cast). HighTemplar: 8 anims,
  PROTOSSHIGHTEMPLAR AI, 45 HP + 40 shield; Purifier: 39 anims incl. rifle aim, 130 HP + 90 shield.

## Thermal sight (directSight) - fixed
- Pipeline (worldEffects_DoPostUIFullScreenEffects, when tPlayer::GetSight = player+0x51C bit 0): things drawn with
  memb_sight* ramp textures (64x1 DXT1; G = heat: occluded ~0.1, cool ~0.55, medium ~0.72, hot ~0.85, critical ~0.95)
  -> GenerateSourceTexture samples pDrawSurface (the BACK BUFFER bound as a texture) -> gauss blur (4 taps/pass,
  weights in c0..c3) -> ProcessWorld: BackbufferCopy (CopyRects) darkened + blurred heat .gb -> ge_sight_lookup
  (512x1 palette in bands) added.
- Bug 1: binding the back buffer as a texture sampled stale Xbox memory (black). HostTexture now StretchRects the
  D3D9 back buffer into a same-size RT texture when bound.
- Bug 2: PSCombinerCount flags: per-stage C0 = bit 12 (0x1000), per-stage C1 = bit 16 (0x10000) (was 16/24). The
  blur's 4th tap took c1 (0.57) instead of c3 (0) -> sum 1.55 -> saturated -> everything at palette end (orange).
- Debug: GHOSTPC_SIGHT=<frame> forces sight on; GHOSTPC_DUMPDRAWS=n,n|auto (with GHOSTPC_TRACE) saves the render
  target after draws to test\drawN.bmp; trace logs PS constants. runtest.ps1 -gameArgs "-level <name>".

## Shadow flicker on distant gunfire (1_2_1b start) - fixed
- Not the cObjectRumbleShake tremor (never triggered there). Distant marine gunfire = muzzle-flash dynamic lights;
  in flash frames the world is drawn through the vertex shader (lit, light=1) instead of fixed function. Overlays
  drawn fixed-function with depth writes off (projected character shadow, blob shadows, decals) then z-fight with it:
  NV2A runs FF and VS on the same unit, D3D9 does not. Fix: ApplyRenderStates adds depth bias -16 units, slope -1
  for FF draws with depth writes off.
- Debug added: GHOSTPC_FRAMESTATS (per-frame draw stats, new-draw diff, all dsound plays, shadow light, ScreenShake
  log), GHOSTPC_TRACE=a-b ranges, GHOSTPC_TRACEJUMP[=x0,y0,x1,y1] (trace every frame into memory, keep frames where
  the draw count or a screen region jumps, with images in test\keep_*), Log capture (g_logCapture).
- Open (ground/corner flicker during gunfire, 1_2_1b start, player at -74,-258,23.5): flash light 1 at
  (-62.7,-287.8,29.1), colour (2,1.2,0.43), atten (1,0.616,0.739), no spot (c119=-1,-1). Game assigns ~6 floor PB
  chunks to it -> drawn with VS vs000 (ambient + 2 point/spot + 2 directional, translation checked: dst/rcp/spot
  ok) on alternate frames. Visible: orange tint on a triangle near the rock wall + sparkle dots on chunk seams.
  Visibility tests (50-55) unrelated. c96..99 for that VS is not written by any API call seen (last writers:
  alpha/skinned/membrane shaders = view-proj); FF neighbours use D3D9 fixed function -> seam mismatch.
- RESOLVED cause (recording 2026-10-24 + campos log): each gunshot nudges the third-person camera ~0.0056 units
  (game logic), easing back over ~15 frames -> sub-pixel image shift -> floor detail shimmer, sparkling polygon
  seams, shadow edge flicker; HUD unchanged; not lighting (GHOSTPC_NOVSLIGHT test identical), not camera shake slots,
  not the VS matrix (c96..99 = current VP x 27962). The game requests 2x multisampling (CreateDevice ms 0x11),
  which GhostPC ignores; rendering is 3x scale without AA. Candidate fix: honour MSAA (render to an MSAA target,
  resolve with StretchRect before Present / when the back buffer is sampled).
- Update: anti-aliasing (4x MSAA, GHOSTPC_MSAA) added - does NOT remove the gunfire flicker. Tint (GHOSTPC_TINTMODE=vsff)
  shows the floor/corner area of the start cave and the right wall switch to defaultWorldLit1/2/3 (vs000/001/012,
  matched by microcode to VertexShaders\XBox; translation checked against the PC .xvu) on alternate frames per
  flash. Remaining cause = route switch FF (D3D9 fixed function, W*V*P) <-> game VS (c96..99 = game VP x 27962).
  GHOSTPC_NOWORLDLIT (draw those via FF) renders them untextured green: the FF decl does not fit that format.
  Proposed proper fix: emulate FF vertex processing with a generated HLSL VS using the game's matrix.
  Shadow overlay bias restored to all FF draws with depth writes off (narrowing to ZERO-src blends brought shadow
  flicker back); GHOSTPC_NOOVERLAYBIAS disables it.
- FIXED (user confirmed): fixed-function vertex emulation. Unlit FVF draws (XYZ position, no vertex blend, texgen
  passthrough/CSN/CSP/CSR) now use a generated vertex shader (FFVSGet/FFVSDecl in d3d_shader.cpp, ApplyFFEmu in
  d3d_core.cpp): position via dot() with c[96..99] when they hold the game's view-projection and the world matrix is
  identity, else own (W*V*P)^T; texture matrices with D3D9 input padding; projected stages put the last component in
  w; fog per vertex like the translated shaders. Lit FF draws (e.g. projected shadows) stay on D3D9 fixed function,
  so the overlay depth bias is still needed. GHOSTPC_NOFFVS disables the emulation. 4x MSAA kept (GHOSTPC_MSAA).

## W-buffer (wall texture fighting, distant walkway distortion)
- The game renders 3D with `D3DRS_ZENABLE = D3DZB_USEW` (logged: ZEnable 2, 0 for 2D). The NV2A stores linear clip w
  with even precision; level art relies on it (flesh layers lie just above rock walls). GhostPC treated it as plain z,
  so the layers z-fought (wall swapping between rock and flesh, worse when moving or up close; distant walkway shimmer).
  Present since the start of the port.
- D3D9 hardware has no W-buffer: emulated in the pixel shaders (`PSDepthWrapper`, d3d_shader.cpp). Vertex shaders (game
  translations and the fixed-function emulation) pass (clip z, clip w, w * 1/(far * k)) in TEXCOORD5, where k is the
  camera-matrix factor (~27962) for clip coordinates from the game's matrix; PS c23 = (W mode, constant depth bias).
  Only draws through our vertex shaders write depth (pre-transformed 2D draws keep hardware z). Slope bias is not
  applied in written depth. GHOSTPC_NOWBUFFER restores hardware z.
- Ruled out on the way: the -0.5 mip LOD bias (sparkle, not the swap), cube-map mip upload, light range cutoff (all 9999),
  vertex alphas of the BLENDDIFFUSEALPHA terrain (constant 255).
- Follow-up: draws with a declaration-only vertex shader handle (no program = fixed-function transform, e.g. the
  refinery bridge in 1_3_1a) still went through D3D9's own transform and kept hardware z among W-depth draws: the bridge
  vanished and large dark-red sheets covered the scene. `DeclFVF` now describes such declarations as an FVF so they use
  the fixed-function emulation too (vertices still converted with the fixed-function declaration). Only pre-transformed
  (XYZRHW) draws remain on D3D9 fixed function.

## Protoss shield shader (checked)
- Shield = game VS shieldRigid/Skinned*.xvu (same positions as the body, oT2.x = dot(to-camera, normal) * c35.x for the
  membrane fade) + shield.ps: t0 height, t1 = texreg2ar(t0) colour lookup, t2 membrane, t3 dissolve;
  r1 = t2 * c0.a; r0 = (t1 + r1) * r1.a; dissolve: r0.a = t3.b - c0.b; cnd(r0.a >= 0.5) r0 : c1.
- Def: PSCombinerCount 0x11106 (6 stages, MUX_MSB, UNIQUE_C0, UNIQUE_C1), PSC0Mapping 0xFFF10FF0 (c0 -> stages 0 and 3,
  c1 -> stage 4), texture modes 0x85E1 (project2d, dpndnt_ar, project2d, project2d). The translation matches.
- Shields look different from earlier GhostPC builds because of the per-stage constant fix (thermal): before it, stage 4
  read c0 instead of c1, so the dissolve fallback colour was wrong. The current look follows the shader's own source.

## Input additions
- D Pad Down (flashlight) also on F; R Thumb (scope) also on the middle mouse button (Click 2). D Pad Up = thermal.

## Enemy Zoo test map (9_1_1a_Enemy_Zoo) and patch 2026-10-25
- New open-sky level built with the OpenNOD SDK 0.6.7, sources in levels\zoo (make_zoo_map.py → zoo.map,
  compile_zoo.bat/.sh run by the user, install_zoo.py post-patches, make_menu_images.py, SDK_ISSUE_REPORT.md).
- Engine facts learned: the single-player player starts at the world origin (gameMultiplayer_PlacePlayer keeps
  the current position; levels move her by script), so the spawn room sits at (0,0,0); spawning inside collision
  gives NaN → tPlayer::OutsideHavokWorldEvent → death. Sky surfaces are drawn unlit with their texture (not
  invisible). Boss AIs (BOSSBRAIN, BOSSULTRALISK) track the player through walls; normal units use HasLOS (Havok
  ray casts). cAIBossSniper crashes outside its arena (sniping spots). Allegiance: cAIInstance::IsFriend treats
  all non-player actors as friends unless ACTOR_AIFLAGS has bit 0x80.
- release\GhostPC_Patch.zip rebuilt (previous one kept as GhostPC_Patch_2026-10-21.zip): GhostPC.exe with this
  session's fixes, Strings\game.nls (+ zoo strings), Misc\ghostsp.nsc (+ zoo mission), the three level files and
  UI\Materials\911a_MenuImage(L).dds. Verified on a clean copy (originals restored from re\backup): zoo level and
  1_2_1b load. The Sabotage Protoss swaps are NOT in the patch (they change an original level file).

## Tanagazj boss AI (cut content, restored in GhostPC)
- Template Tanagazj asks for ACTOR_AI BOSSTANAGAZJ, but aiClass_Startup never registers that name (no string in the
  exe), so he spawns with no AI. cAIBossTanagazj is complete code: chase, blade melee, ranged, cloak, shield, boss bar.
- It is woken only by a mission script: ScriptNotify 101 = phase 1 (targets the player, boss bar), 102/103 = later
  phases, 200 = end, 311-314 other events (jump table at 0x4F24FC). InitInstance pins him in place until then.
- Two bugs explain why it was left unregistered: PreUpdateAIState calls cAIInstance::UpdateAIState instead of
  cAIBaseWalker::PreUpdateAIState (endless recursion → stack overflow), and it overrides HasShield but not GetShield
  (UpdateShield crashed in cProtossShield::Update; its shield lives at this+0x5AC). No special entities or zones needed.
- src/levelscript.cpp registers the class like aiClass_Startup (0x28-byte cAIClass, prototype from NewInstance), patches
  both bugs (vtable +0x538 → this+0x5AC; the call at PreUpdateAIState+4 → cAIBaseWalker::PreUpdateAIState), and runs
  per-level scripts Levels\<level>.gpc ("notify x0 y0 z0 x1 y1 z1 <ai class> <msg>"). GHOSTPC_NO_TANAGAZJ_AI disables
  the registration. Verified: message 101 reaches him and the game keeps running.
- Psi blade: phase-dependent, not a model or entity problem. 101/102/103 set the phase field (this+0x594) to 1/2/3;
  BladeOn (effect "psiblade" on model marker type 3 index 0, which Tanagazj.nod has on bone 7) is only called from
  ASUpdateAcquired/ASUpdateCombatRanged while the phase is 3, and the idle/patrol states drop it. Phases 1-2 fight
  bare-handed (phase 2 adds the arena-platform sequence). The zoo starts him with 103.
- Cloak (intended, all phases): every phase message arms it (+0x56C = now + 4-8 s); UpdateCloak then alternates
  Cloak (4-8 s: terran_ghost_cloak01 sound, model parts disabled, "playercloak" shader) and UnCloak (6-10 s visible).
  Fully invisible is intended: while cloaked UpdateCloak sets tThing +0x48 bit 2 (tThing::Draw skips the thing) unless
  the player has thermal sight (tPlayer::GetSight), then switches the shader to its visible mode. Debug
  GHOSTPC_DEBUG_TANAGAZJ_SHIMMER skips that write: the cloak shader then draws him correctly (refraction shimmer).
- Cloak shader (cCloakShader, "playercloak"/"observercloak"/"generatorcloak"/"ghostcloak"; renders correctly in
  GhostPC): refraction of a back-buffer copy (rlMain_GetBackBufferTexture: CopyRects into "BackbufferCopy") offset by
  the view-space normal (VS: oT0 = screen position + normal * c129.x), plus a rim term. RenderMeshGroup draws models
  other than the player's only within 8 m of the camera (full under 5 m, fading 5-8 m), so cloaked NPCs are invisible
  farther away by design. Fade level +0x70 ramps to +0x6C (cCloakShader::Update). The Shuttle uses the Observer's AI
  (PROTOSSOBSERVER, observercloak: refraction only, no membrane texture): both shimmer within 8 m (verified).
- Damage: his blade hit is MeleeHitCheck(..., 10) per hit, a third of a Zealot's (BladeHitCheck: sphere check, 30).
  GhostPC raises it to 80 (FixTanagazjClass), as placeholder tuning of the cut boss. (game.nfx is loaded
- Blade keeper (GhostPC): BladeOn only runs in ASUpdateAcquired/ASUpdateCombatRanged; fights that stayed in his other
  combat states never drew it (the user's walk-in encounters). Every 10 frames, Tanagazj in phase 3 with a target, not
  idle (state 2) and without a blade gets BladeOn (levelscript.cpp KeepTanagazjBlade).
- Level scripts are found by the loaded level's name. mainOptions_aLevelName is only set by "-level"; levels started
  from the menu left it empty, so their .gpc never loaded (no triggers: Tanagazj stayed in phase 0, bare-handed; no
  player_start; no props commands). A pass-through detour on gameWorld_LoadWorld (prologue sub esp,0x22c moved to a
  trampoline) records the name for every load.
- Ultralisk boss (cAIBossUltralisk; its original arena had towers, depots and a psi emitter): ultralisk.nod in this
  build has no model markers, which broke it. GhostPC (levelscript.cpp, all levels):
  - LOSCheck (CHASE -> AQUIRED only on line of sight) cast from the marker-4 fallback, 1 m above its origin inside its
    own body, so it never acquired Nova: replaced by a ray from 5 m ahead (tThing +0x90 facing) at 3 m to 1 m above
    the target's feet.
  - Tusk hit (AnimationCallback: MeleeHitCheck markers 3 -> 5) never connected: replaced by a 4 m sphere 5 m ahead
    of it, one hit per swing (600 ms), 100 damage (the user's tuning; game 25).
  - It pins itself (InitInstance) and only its mission script released it: level script "unpin cAIBossUltralisk".
  - Charge (SPECIAL1, SteerVelocity) did not move it: while the charge stage (+0x584) is 2 it is moved towards its
    target at speed (+0x37C) x 1.5 (SetPhysPos + MoveToPos), stopping 5 m short.
  - Stomp (COMBATRANGED ground attack): cooldown 2 s -> 5 s (StopAttackRanged), and 20 damage with its knockback.
  Verified: acquires at 14 m, charges in, tusks hit.
- Level restarts can reuse the tWorld: the gameWorld_LoadWorld detour counts loads and the level script reloads on a
  new count; player_start waits until the (new) player stands at the world origin.
- Level script "props_player_only": cAIDemoAnims::Damaged (vtable +0x190) is wrapped; with the command, props cycle only
  when the player damages them (the Scout's fire kept cycling the Reaver).
  whole; the .nlfx list does not limit effect definitions.) Without any notify (phase 0) he still fights once shot,
  bare-handed: that is what happened when he was shot through his door from outside the trigger box, so .gpc notify
  commands also fire as soon as an actor of the class is below full health (tActor +0x36C health, +0x370 maximum).
  Verified with screenshots: blade drawn in phase 3, and the damage trigger sends 103 when shot from outside.
- GHOSTPC_TANAGAZJ_DEBUG=1 (force the notify, shoot him, log AI state +0x18 / phase / blade flag / attached effects) or
  =shoot (shoot only); GHOSTPC_TANAGAZJ_DEBUG_POS="x y z [yaw]" warps the player first (tThing::JumpToMatrix, as
  cCueRow::WarpPlayerIntoPlace; tThing::MoveToPos only moves the shadow).
- Debug: GHOSTPC_LOGFIRSTCHANCE logs access violations/stack overflows before any handler (with a stack guarantee).
- Sniper boss (sniper_boss "Aries", BOSSSNIPER = cAIBossSniper; his level is stripped from this build, no NSD places
  him). He never walks: aSniperSpots (static, 3 x 0x3C tSpotInfo) is MT (middle) linked to RT and LT, each link
  (target spot, motion A, motion B) = special1-8 (SniperMTtoRT, SniperSneakMTtoRT, ... LTtoMT), which FastMove plays
  with root motion (MoveRootWorld); action1-9 = stand / snipe right / snipe left per spot. The animations are in
  arena space relative to his placement (root bone = feet; bone 0 rests at the origin): MT (0.40, 21.06, 21.07),
  RT (-21.71, 22.96, 27.47), LT (22.56, 22.83, 27.70), facing +Y pitched down. HasLOSFromSpot reads the spot's
  position from his placement's track: frame[spot + 1] (tThing +0x134, 28-byte frames: rotation, position, uint32,
  as tThing::TrackToFrame reads them); with no frames that pointer is null, the crash seen in the zoo before.
  ScriptNotify 0/1/2 sets a mode (+0x566), enables the boss bar and targets the player.
- His moves are in absolute world coordinates: tObject::MoveRootWorld sets the actor to the animation mixer's root
  position as is (verified: on MT his actor is at world 0.4 21.1 21.1 whatever his placement). So his arena must be
  at the world origin. Zoo: the level shift (OFFSET) puts the arena's origin there; the player, created at the world
  origin, is moved to the spawn room by the level script ("player_start"). Pillars topped at the three perches, him
  placed on MT, install_zoo.py inserts the four frames (MT, MT, RT, LT) into his NSD placement (MAPtoNSD writes
  none). Trigger: notify cAIBossSniper 0 on entering (or shooting him). In the first test (arena elsewhere) he cycled
  MT -> LT -> MT -> RT -> MT without crashing.
- He only attacks (ASUpdateAcquired -> state 12) when a phase-weighted roll says stay and HasLOSFromSpot succeeds;
  otherwise he relocates. HasLOS casts a ray of his sight distance, 40 m (GetCurrentSightDist = +0x330 from Clear),
  from the spot's eye: with Nova 45-55 m away he never saw her and only jumped around. The zoo courtyard is therefore
  only ~25 m deep. While attacking, UpdateAttackRanged keeps him on the snipe animation's root (MoveRootWorld), and the
  snipes step 2-5 m aside (leaning out from cover): perch platforms are sized from the stand + snipe root keys (he
  fell off 4 m pillar tops).
- He faces south (-Y), not north: the snipes step to his right (-X on MT) and forward (-Y); the side towers lean in.
  So the courtyard lies south of the perches, around the world origin. Perches are catwalk pads along his
  stand/snipe root paths on a column under each stand spot: any floor more than ~2 m in front of a stand spot blocked
  HasLOSFromSpot (eye 3.0 m, GetEyeZ; steep ray down into the courtyard). He fires (FireSniperProjectile via animation
  tag 4, FireInstantProjectile). Offline ray check (make_zoo_map.py geometry): every spot from 6 m south of the origin
  up to the perches is seen by at least one perch; the south end near the door is out of range.
- Zoo layout (2026-10-25): sniper arena north of the Vulcan/Hauler rooms, reached from their L hallway by a corridor
  north between the Hauler and Zerg rooms; Zerg area extended north (nydus canal, sunken colony); Lurker boss room
  north of the Ultralisk room (BOSSLURKER: any ScriptNotify but 10 targets the player; trigger on entering);
  Tanagazj's room entered by an L hallway inside his trigger box.
- NAD v4 as found here: 16-byte header (version, track count, flags, duration in frames), then per track a 12-byte
  header (key count, bone, type 0 rotation / 1 position) and 52-byte keys (frame, value, b, c, d) - the SDK doc's
  8-byte track header is wrong for these files.
- Probe facing (fixed in GhostPC for all levels): template AI PROTOSSPROBE is cAIDetector (a cAIBaseFlyer; also
  registered: PROTOSSPROBEOLD = cAIProtossProbe, a wanderer, and PROTOSSPROBE2 = cAIProtossProbe2, both ground walkers
  that need walk animations the probe has no motion set for). It aims its desired yaw at Nova (cAIBaseFlyer::Face,
  +0x3A4) but cAIBaseFlyer::UpdateFacing only turns with AI flag 0x10000 at rate +0x3A8, which cAIDetector never sets
  (cAIProtossObserver::Clear sets 0x10000 and 300 deg/s). levelscript.cpp wraps cAIDetector::InitInstance (vtable
  +0x20) to set both. Verified: yaw now follows the desired yaw.
- Similar: Artemis's BOSSARTEMIS has no class at all in this build. Her artemis.nod is an older 32-bone skeleton and
  every motion set naming it (artemis.nms: marine/Nova animations; nova.nms for artemis_prop) plays animations for
  other skeletons, giving stretched shards. The zoo places Artemis_cut with a map-only motionset override
  (nod_prop_13 = dropship_cut, the masked Nova's set, whose only animation never plays), so she stays in bind pose.

## 2026-10-26: Sunken Colony, Lurker, custom sounds, restarts, zoo additions
- Restart after death: tSequence::DoRestartSinglePlayer restores the level-start snapshot (cSaveData::Restore) instead
  of reloading the world. Restore is detoured to count as a load, so the .gpc runs again (player_start, triggers, unpin).
- Ultralisk stomp: AnimationCallback hits a grounded target (|dz| < 0.5, any distance) with Knockback (+0x564) or
  Hurlback (+0x56C); both calls are wrapped to add 20 damage (only Knockback was before).
- Sunken Colony (global fixes):
  - CMUpdateSpecial1 fired again every update after clearing its spike time to 0 (now > 0): several 150-damage spikes
    per attack. Wrapped to skip while the spike is spent and the animation still runs.
  - proj_SunkenColony was invisible: projectiles face their velocity each update (PrePhysicsUpdateProjectile ->
    LookAt3d/LookDir3d) and the spike has none, so its rotation became all zeros. Zero/NaN directions are now skipped
    (FixStillProjectiles, all projectiles).
  - The spike's rise is root motion (stand anim: +15.66 m at frames 11-14, down by 23), which projectiles never apply;
    DriveSunkenSpikes moves it, starting at frame 11 so it comes up with the damage.
  - Range: sight 80 m (+0x330), strikes up to 60 m, no line of sight. The .gpc command sunken_los adds one (zoo only).
  - Silent: plays "swing" at CMStartSpecial1 and "hitdirt" at the spike (custom WAVs).
- Lurker boss: its 150-damage "early spike" was the Sunken Colony (then at map 153,222) striking through the wall; the
  Lurker's own spike (proj_lurker, 25 damage) followed. Its bank zergLurker (shared by lurker_boss, lurker_cut,
  lurker_boss_arclite; the props have no foley) holds only footsteps and special1/2, and its code plays nothing else.
  GhostPC plays: vox_alert 3 s into the roar (CMStartSpecial4, call for help), vox_death (Killed), special1/2 (burrow/unburrow),
  swing (FireSpike), hitdirt where its spike ends.
- Custom foley (src/customsound.cpp): Sounds\custom\<foley set>\<mode>[~n].wav replaces/adds cues; tObject::
  PlayFoleyMode is detoured (the set's name is the first 32 bytes of tFoleySet at thing+0x28C). Distance falloff from
  the player (5..60 m). GHOSTPC_LOGFOLEY logs every PlayFoleyMode call. Hauler/Vulcan banks (bossHauler, bossVulcan)
  do not exist in this build.
- Nova_DT (nova_DT.nod, darkTemplar_eye_fx): only its own template references it; no level or code uses it.
- Zoo: Goliath_232 arena (TERRANGOLIATHBOSS = cAITerranGoliathBoss, engages on its own; no trigger) by the sniper hallway; hauler_cut, spiderMine, AR_psiDampener (wall),
  landed dropship, Scout 14 m up; Sunken/Nydus south, CC/Spore north; ammo + grenades by every boss door.
- Mutalisk (cAIZergMutalisk: the Scourge's AI plus a follow-path state, state 59, the only one it attacks from) flies
  only on a path a mission script starts: cAIBaseFlyer::StartPathMove(track, next track, rate) plays a .nad as movement
  (tRenderKeyMixer::PlayMovement: positions in world coordinates turned by the actor's facing; refuses tracks with more
  than 2 tracks, so a path file holds only the root rotation + position; rate = key frames per second). StartPathMove
  does not check PlayMovement's result. The zoo's zoo_mutaliskFlyAround is 1_2_1's FlyAround loop scaled 0.59 into the
  Zerg room at 30-42 m (over the 26 m Command Center), started by the .gpc command fly_path <class> <track> [rate].
- Goliath_232's template runs its mission script (script cM232_GoliathMain): BeginScene plays 232a_intro.ncs and
  notifies the 232COLUMN1-8 cast, Killed calls ScriptSequence::NextMission. In the zoo the intro's cast is missing and
  the game crashed as the cutscene ended (cCueRow::PostPlay on a thing without a model). The zoo placement overrides
  modifier 15 (script) with "none" (script_New finds no class and returns null).
- sunken_los now tests the line to the Sunken's target (+0x2D4), not the strike point: a target past 60 m is struck 60 m
  along the way, which was a visible spot inside the Zerg room next to Nova in the corridor (the "random" hit sound).
- Lurker burst attack (CMStartSpecial1, spawns lurkerunburrow) plays special2 (the unburrow WAV). No game bank has a
  ground-burst cue (only expRockfall/propRockFall explosions and dirt scuffs).
- Zoo: the psi dampeners hang on walls where placed (they only looked dropped while their RAISE entries were lost).
- Overlord (cAIZergOverlord) losing sight of its target: it goes back to patrolling (state 5; see "Flyer states" below
  for the real cause) and cAIBaseFlyer::CMUpdateFly
  (controller mode 3) steers it straight at the target with no navigation, sinking to the floor and into walls. Its
  CMUpdateFly is wrapped: never below its starting altitude, a step blocked by a wall is undone. (Observers did not
  show it; the Probe walks.)
- Zoo last round: scvDriver by the scientist, siege tank faces west (away from the door), Hydralisk by the Command
  Center, Goliath cover reduced to four pillars.
- Cut Goliath boss (cAIBossGoliath): GhostPC registers it as BOSSGOLIATH (RegisterMissingAIClasses, like Tanagazj;
  NewInstance builds a full 0x3E8-byte instance = GetMem). The zoo gives it to Goliath_222c (Goliath_rebel model
  goliath_boss.nod, motion set goliath_222c.nms) with a map-only ACTOR_AI override (modifier 43), in a second arena
  east of the sniper hallway, woken by notify cAIBossGoliath 0 (boss bar + target). Its code plays walk/run/stand/
  idle/firestand/turn180/knockback/blowback; goliath_222c.nms lacks run, idle, knockback and blowback.
- menu_bc = the menu scene's Battlecruiser (materials BattlecruiserHull / Battlecruiser_glo, _TerranCutscene).
  Its .nod header bounds are ~1504 x 1779 x 482, about 100x the template's editor box (15 x 18 x 8 m): check its
  in-game size.
- Zoo: the light limit is 64 (both Goliath arenas share one larger light each); squad side room SQUAD in the Terran
  room's north-west corner (open top, 4 m walls) with the eight named marines.
- Cut Goliath boss, working: cAIBaseBossGoliath::FightToFrame(n) (vtable +0x8F4) walks it to its placement's track
  frame n (+0x134, 28-byte frames, position at +0xC) in RUNMG mode (14), then it attacks (ATTACKRANGED2, rocket
  volleys ~52-60 damage each at rocket frames 1-3/5-7) and reports scriptMessage_AIArrived from ClassUpdate. Its
  mission script's messages map to frames (100->9, 200->5, 201->3, 202->2, 203->1, 204->7, 205->6; 400/401/410
  resume/stop). GhostPC's DriveGoliathBoss stands in for the script (random frame 1-9, 3-5.5 s after each arrival);
  install_zoo.py writes its 10 frames. Controller mode names: NONE, DEAD, STAND, WALK, RUN, LOOKAROUND, TURN180,
  COMBATSTAND, ATTACKRANGED, ATTACKRANGED2, ATTACKMELEE, ATTACKBLIND, KNOCKBACK, BLOWBACK, RUNMG.
- zoo_goliath_222c.nms (copied to 3D\Motions, set on the placement with modifier 13): goliath_222c.nms + run/idle from
  goliath.nms (same 29-bone track layout) and knockback/blowback = goliathBoss_bigdamage.nad (Goliath_232's).
- TerranVulture: its template asks for TERRANVULTURE, which aiClass_Startup never registers (the string is not even in
  Ghost.exe), so the Vulture had no AI in any level. cAITerranVulture (cAIBaseVehicle; NewInstance 0x3C8 = GetMem) is
  complete; GhostPC registers it with the other missing classes. Tested: it chases Nova (state 8) and hits her with
  explosive shots (10 + ~30 blast, then burning).
- Vulture through walls: cAITerranVulture::Search takes a target when IsValidTarget (+0x15C) and IsTargettable (+0x158)
  pass; the Vulture's IsTargettable override checks range/direction only (the base cAIInstance one also runs the
  sight checks). Wrapped globally with a line of sight (1.5 m above the Vulture to 1 m above the target). Tested:
  Nova 27 m away behind the Terran south wall -> stays idle; in view -> chases.

## 2026-10-26 (later): mouse input
- Controls: Y = aim; while aiming R thumb = sniper scope; in the scope L thumb steps the magnification. Outside aim
  mode R thumb crouches / L thumb stands (verified with scripted presses).
- The middle click did nothing although bound (R Thumb = M | Click 2): presses are now latched until the next
  XInputGetState poll has seen them (a quick click could go down and up between two polls), and WM_MBUTTONDOWN/UP +
  WM_XBUTTON* feed the same state for mice whose wheel click raw input misses. GHOSTPC_LOGINPUT logs mouse buttons.
- New default: L Thumb = B | Wheel+ | Wheel- (a wheel notch = one L thumb press = one magnification step).
- Cut Goliath machine guns: FireGoliathMachineGun (vtable +0x9B0) = gameProjectile_InstantFire from gun marker 0-2,
  5 damage, 100 m, aimed at the target +0.7 m with a random yaw/pitch of +-10 deg x (1 - accuracy); the boss passes
  0.5 (+-5 deg): 1 hit in 36 shots at ~20 m. GhostPC wraps it with the spread at 40% (0.5 -> 0.8, +-2 deg): 9 of 36.
  Debug: GHOSTPC_NO_GOLIATH_ROCKETS turns its launchers off (FireGoliathLaunchers returns at once).

## 2026-10-26 (later): Protoss Scout (cAIProtossScout; the Wraith uses the same AI)
- Flight: ASStartAcquired aimed a point 60 m beyond the Scout and 40 m above the target (+0x2E0, controller mode 4),
  ASStartCombatRanged a firing pass to 2 m past the target, 10 m up: open-terrain attack runs; indoors it hit walls and
  climbed to the wall tops. Replaced (GhostPC, all levels): acquired = standoff 9-14 m out on its side, 7 m up, with
  line of sight to the target and a clear flight (other directions / shorter distances if not); combat = a ~35 deg
  slide around the target. A combat timeout went idle (push 2 at ASUpdateCombatRanged+0x56): now acquired (11).
- Gun: FireWeapon = FireInstantProjectileAtPos, 20 m range, aimed at a point moving from +0x3E4 to +0x3F0 over the pass
  (a line laid for the fly-over). Now re-laid every frame across the target at chest height, +-0.5 m. Test: 10 of 92
  shots hit (was 4 of 135).
- Lockdown: cAIProtossScout::Lockdown just called tActor::Kill (exploded; once crashed), UnLockdown (+0x1A8) was an
  empty stub. Now like the Observer: OverridePatrol, model +0x14 |= 0x20000, ZeroVelocity, state 54, controller 2,
  shield (+0x3B4) off; UnLockdown reverses it and goes curious (state 4) at the target's position, or idle.
  Debug: GHOSTPC_TANAGAZJ_DEBUG_LOCKDOWN=<frames> locks the watched AI.

### Flyer states (2026-10-28)
- AI state numbers: per-state start/update functions sit at vtable +0x5D0 + 4*state / +0x6C8 + 4*state (idle 2 at
  +0x6D0, curious 4 at +0x5E0/+0x6D8, patrolling 5 at +0x5E4/+0x6DC, acquired 11, combat 12, lockdown 54, Mutalisk
  follow-path 59). Earlier notes calling state 5 "curious" were wrong: 5 is patrolling.
- Patrolling flyers steer (mode 3, at +0x2E0) toward the patrol point +0x25C until within 3 m, then hover. Flyers
  placed without a patrol route keep the 0xCCCCCCCC fill there (~-1e8): they flew off toward that far corner, low and
  into walls (the Overlord's "dive" after losing Nova, the Scout after lockdown). Fix: cAIInstance::ASStartPatrolling
  is replaced (jmp): after RECOSet(5), a flyer (vtable OverridePatrol = cAIBaseFlyer's) with no patrol point gets its
  placement position (tThing +0x350) as patrol point and goal: it flies back there and hovers. Path flyers (the
  Mutalisk's fly_path, state 59) never patrol and are untouched (checked: it loops its path before and after Nova
  engages and leaves).
- Scout curious (state 4) was the empty stub: now flies to a lookout 3-7 m above the curious point (+0x40, set by
  ALSetCurious), hovers facing it, reacquires after seeing the target for 1 s (from 1.5 s in), idle after 10 s.
  The Scout's own AI never enters patrol or curious otherwise: losing the target it goes acquired -> idle and hovers
  where it is (idle runs Search every second).
  Patrol-home is limited to cAIProtossScout (Scout, Wraith) and cAIZergOverlord (user: not the Shuttle, Observer...).
  Scout/Wraith idle for 2 min away from their placement -> patrolling (home). GHOSTPC_SCOUT_HOME_MS shortens it.

### Crash diagnostics (2026-10-28)
- Crash report (loader.cpp CrashFilter): exception, registers, thread (game/other), frame, the GhostPC step that was
  running (g_crumb, set through ShellWatch/LevelScriptUpdate), stack scan with Ghost.exe symbols and raw GhostPC
  addresses (resolve with build/GhostPC.map, base 0x10000000), and ghostpc_crash.dmp (minidump) next to the log.
- Hang watchdog: no frame for 15 s with the window focused -> "HANG" + the game thread's stack.
- crashcheck: world pointer changes, level load/restart lines, and every 60 frames / each load the per-unit tables
  (Goliath boss, Lurker roars/spikes, Overlord altitude, Scout search/idle, debug watch) are checked against the
  actors in the sectors; stale entries are logged and dropped.
- GHOSTPC_TEST_CRASH=<frame> faults on purpose (checks the report); GHOSTPC_TANAGAZJ_DEBUG_FRAMES sets how long the
  watched AI is logged.
- Exhaust: game.nfx scoutThrusters = 8 x scoutThruster on markers type 17 index 1-8; scout.nod has no markers (all drew
  at the origin). cModel: name +0x8C, markers +0x128 (0x50-byte tModelMarker: matrix, +0x40 bone, +0x41 type,
  +0x42 index), count +0x12C. GhostPC adds 8 markers at the four tail vents found from its vertices, turned 180 deg about z so the flames point back (x +-0.8, z 4.4 /
  2.8, y -8.0/-8.1; NOD layout from the OpenNOD SDK's import_nod.py). GHOSTPC_DEBUG_MARKERS=<type|-1> logs markers.
- Debug picker (GHOSTPC_TANAGAZJ_DEBUG=shoot) now takes the instance nearest the player.

### Zoo fixes (2026-10-28, later)
- Scout/Wraith (cAIProtossScout): goals must be in its room (within 30 m of its placement, clear line from it, at most
  4 m above it); no valid standoff point -> hovers and lets acquired time out (+0x3D4) instead of the game's goal 60 m
  out / 40 m up. Kept 3 m above the target's feet by its physics body's velocity (tThing +0x158 hkEntity, motion +8:
  velocity +0x40, set via vtable +0x50; SetPhysPos did not stick). Lockdown: physics off (SetPhysActive 0), the
  target's position recorded, searched on unlock (curious) or idle. Return home timed per frame (DriveScoutsHome):
  2 min after its last fight/search. Gun line +-0.75 m.
- Lurker: proj_lurker ran along the floor through Nova without colliding (no Damage call): a live spike within 1 m
  of her hits once for 75 (TrackLurkerSpikes). DoBurrowedDamage amount (immediate 25.0 pushed in CMStartUnburrow and
  AnimationCallback) -> 90.
- Purifier beam: the purifierBeam controller's parts (cEffectLight, cEffectParticleSmoke "purifBeam", elec, 3 damage
  per particle via ParticleCollideRay) keep cEffect::SetTargetThing (a no-op), so the stream followed the gun's aim,
  above Nova: no hits. cEffectParticleSmoke homes its particles on +0x140 when +0x104 & 0x2000000 (SetTargetPos sets
  both): FirePurifierGun is wrapped to give each part the target's chest (feet + 1 m). Tested: ~30 damage/s.
  (cEffectPurifierBeam exists but is not what this controller uses.)
- Test aids: GHOSTPC_TEST_GOD=1 keeps the player's health (+0x36C) full (the Scout kills Nova in Protoss-room tests);
  put Nova in front of the unit's facing (see ZOO_OVERVIEW.md: world coordinates of everything in the zoo).
- Antivirus: a heuristic (Malware.Heuristic.3029) quarantined builds that resolved MiniDumpWriteDump with
  LoadLibrary/GetProcAddress and had the SendInput test click / thread-suspending watchdog; dbghelp is now linked,
  SendInput removed, the watchdog only logs.
- Profiles (shell.cpp): single player could only make one profile (Menu_SelProfileSingle mapped Y to state 0, prompt
  box empty; co-op maps Y to 0x25 = make profile): patched to match, "{y} Create New". Deleting was cut from the
  menus but not the shell: events 0x45B + index -> State_DeleteProfile_Start -> Menu_RUSureDelete ("Are you sure...",
  No preselected) -> State_DeleteProfile. cShell::UpdateInput passes A (stdInput 4), X (1), start (11) to the profile
  list; a cShellButton fires +0x6C for +0x69/+0x6A and +0x70 for +0x6B, but cShellProfileSelect::Init set
  A/start/start with both events = load. Wrapped Init: X -> 0x45B + index, "{x} Delete" (MNU_X_DELETE) in box 3.
  Tested on a backup of save\U: the profile's folder is deleted, the list returns. Zoo unlock: IsMissionUnlocked
  (NSI/NOVA names or the unlock cheat unlock all) also returns 1 for mission ID 9_1_ZOO.
