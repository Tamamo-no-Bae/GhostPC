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

## 2026-10-29: Multiplayer in the build (investigation)
- Local only: one source file, game\gamelib\gamemultiplayer.cpp (PDB source list); no game network code at all.
  XNet is linked but only hkBsdSockServer (Havok visual debugger) opens sockets; pXBoxLiveEngine (ILiveEngine, an
  empty forward-declared type) is never read or written.
- Two local players, split screen left/right: statics pLeftLocalPlayer 0x828528 / pRightLocalPlayer 0x82852C
  (cPlayerInfo *, named in the PDB; the map labels them gameMotion_apNames+728/+72C). LocalCameras_UpdateSplitScreen
  Dimensions halves the width (tRenderCamera::SetCanvas, cPlayerInfo +4 = tSimCamera, +0x250 render camera);
  UpdateControls runs simControl per player after stdInput_SetCurrentGamepad(info +0x10 pad). Max 2
  (CmdNewPlayer refuses a third). Per-player HUD, a score display (cUIScore), MP branches in cPlayerControl::
  UpdateDead, tActor::Kill/Revive, DropPowerup.
- Shell: Menu_Multiplayer (menu 28: Cooperative -> state 0x29, Competitive -> 0x2B), Menu_SelProfileMultiCoop/Comp
  (20/21, "Select Profile for Player 2"), Menu_MultiCoop/Comp (31/32, mission select). States 0x22/0x23/0x24 open
  menus 28/31/32 (UpdateState jump table, index = state - 1; menu table in LoadMenu, id = index + 1). Nothing raises
  0x22: Menu_Main has only Resume Campaign (0x27), Options (0x2C), Debug (0xE, mainOptions_bDebugMenu). Co-op/comp
  need 2 profiles (P0 then P1 via State_LoadProfile) and load ghostmpcoop.nsc / ghostmpcomp.nsc (gameChronicle_
  LoadMPCOOP/COMP), which are not in the build (Misc has ghostsp.nsc, ghostsp2.nsc). The second-player flag is
  cShell +0x6E34 (gameMultiplayer_BeginGame creates a second player when set; State_SelectProfileSinglePlayer
  clears it). No MP levels: Levels has the SP missions only.
- Test (GHOSTPC_TEST_P2=<frame>: CmdNewPlayer in a running level): the screen splits, both HUDs and score counters
  draw, but the world renders black in both halves (GhostPC's D3D9 layer and the split canvases), player 2 spawns
  at the world origin. CmdNewPlayer left the new camera's region (tSimCamera +0x188) at 0xCCCCCCCC (a level load
  sets it): crash in UpdateCameraRegion; the test clears it.
- Player 2 (and player 1) in every mode: gameMultiplayer_CreatePlayer(mainOptions_aPlayerTemplate = "nova", the
  default; a hashed command-line switch can change it), same stats/weapons/powers; gameMultiplayer_New takes the
  first free of two 0xA4-byte cPlayerInfo slots (0x8283D0); cPlayerInfo::Init builds only camera and HUD. No
  per-player skin, model or tint anywhere: player 2 is a second, identical Nova.
- Nova templates: Nova_red, Nova_bl, Nova_br, Nova_ice (based on Nova / Nova_red) set nothing of their own (no
  model, material or shader: placeholders; templates have no material/skin property at all, only editorcolor).
  NovaReduxFinal names novaReduxFinal.nod (not in the build; nova.nod itself uses NovaReduxFinal_* materials).
  Nova_DT = nova_DT.nod (Dark Templar armour: textures NovaDarkTemplar.dds / Nova_DT_New.dds exist, its six
  materials Nova_DT_* are defined in no .nsa). Nova_Artemis = artemis.nod.
- Skeletons (NOD header byte 5 = bone count, 64-byte bones, parent at +62): nova.nod 43 (face bones under the head,
  plain hands); nova_DT.nod, novaFaceCovered.nod, artemis.nod 32 (the old skeleton: 43 = 32 plus face bones, the
  arm/leg chains map one to one, so nova_DT could be converted to the new skeleton); Ghost_male.nod 40 (no face,
  jointed fingers, different hierarchy). nova.nms 137 modes vs ghost_male.nms 42 (22 shared; no ladders, poles,
  ledges, snipe, melee, psi): the male Ghost as a player is tabled. About 25 Nova animations reach only bone 31
  (old-skeleton leftovers).
- Player starts: tWorld +0xF0/+0xF4 and a second list +0x100/+0x104 (tPlayerStart 0x60 bytes: number +0x50, order
  +0x54; loaded by gameScene_Load). cPlayerInfo::BeginLevel places the right-hand player with order 1, the left with
  0 (PlacePlayer -> FindPlayerStart(number, order), falling back to a start with the same number). Every shipped
  mission has only order-0 starts (1_3_1a also number 9), the second list is always empty, the zoo has none
  (GHOSTPC_LOG_STARTS=1 logs them): no co-op starts anywhere; player 2 would start on player 1.
- Competitive: the same menus as co-op (profile per player, mission select); no character, colour or team choice.
  ghostmpcoop/comp.nsc may set "gametype" (cChronicle +4) and "profileid" (+0); gametype is only read by
  cShellMissionSelect::Init (mission pictures). No player model options in any mode.
- Player starts in the files: in the .nsd, after the entity records (the trailer's "auxiliary world arrays", which
  MAPtoNSD writes as zero counts: the zoo has none), a uint32 count and count x 0x3C-byte tPlayerStartFile records
  (sectorID, 4x3 matrix: rotation rows then position, number, order). gameScene_Load reads the count and the records
  sequentially (stdFile_Read, no offset table), so a record can be inserted (count + 1). Found by position: e.g.
  1_2_1_Miners_Bunker.nsd 0x4AC9D (count 1 before it, sector 12, number 0, order 0, at -82.6 -171.0 0.125).
- Level transitions with two players: cLevelChange (region script; Entered / Triggered) calls
  ScriptSequence::ChangeLevel -> tSequence::ChangeLevel / DoChangeLevel: TerminateLevel (gameMultiplayer_EndLevel:
  both cPlayerInfo::EndLevel; PlayerEndLevel frees effects/physics, WorldRemove - the players survive), StartLevel
  (LoadMap, then BeginLevel(world, start number +0x90, flags): both players placed at their start with that number,
  order 0 / 1), then gameMultiplayer_LocalChronicle_SetPlayerData -> cChronicle::SetPlayerData for both. Entered
  only reacts to ScriptPlayer::IsPrimaryPlayer (a player-controlled Nova on foot: GetOldPlayer empty; either
  player) or a tank/Goliath/Vulture: either player moves both to the next sub-level. Each start number that a level
  change targets needs an order-1 start for player 2 (else he lands on player 1).
- Death with two players: cPlayerControl::UpdateDead, when not single player, revives the dead player in place at
  full health (tActor::Revive(1, 1)) on a button press of his own pad; restart / save / death menus are the
  ...SinglePlayer functions only.
- NOD layout (confirmed on nova.nod / nova_DT.nod): header 92 bytes (byte 4 shaders, 5 bones, 6 vertex sets, 7 mesh
  groups), 32-byte shader names, 64-byte bones (rest position, 3x3 rotation, inverse-root translation +48, parent
  +62), vertex data, indices, 56-byte mesh groups (cMeshGroup::ReadFile: shader index +0, LOD ranges +4, flags
  +0x1E, bone palette aBones[20] +0x21, numBones +0x35, vertex set +0x36; vertices index the palette), marker count +
  52-byte markers (bone, type, uint16 index, 4x3 matrix), 4 zero bytes.
- Nova_DT converted (scripts/convert_nova_dt.py -> game\3D\Models\nova_DT43.nod, a new file): nova.nod's 43 bones
  and 39 markers, DT's palettes renumbered (old 0-6 same, ponytail 7-13 -> 9-12, arms 14-21 -> 25-32, hips/legs
  22-30 -> 33-41, 31 -> 42; the rest positions are identical; DT uses no ponytail bones). Tested as the player
  (GHOSTPC_PLAYER_TEMPLATE=Nova_DT with the file in place of nova_DT.nod): animates correctly with nova.nms, rifle on
  the back; untextured until its six Nova_DT_* materials are defined.
- Nova's textures for reskins: scripts/export_textures.py -> exports\nova_textures (colour / alpha / rgba PNGs; the
  suit's alpha is a gloss/glow mask, which is why viewers show it near-black).
- Materials: cShader::LoadArchives (worldEffects_Startup, unless bundles mode) opens each archive named in
  Materials\shaderlist.txt with cShader::OpenArchive(name, 0x32), which indexes every material name in
  cShader::pHashtable for loading by name. GhostPC opens one more, Materials\ghostpc.nsa (source in
  assets\Materials), once that table exists (calling it on frame 0 crashed in stdHashtable_Find). It defines Nova_DT's
  seven materials, textures chosen by where each material's UVs fall (scripts/nod_uvmap.py draws them over textures):
  Nova_DT_New = armour plates (Nova_DT_Armor_gloss), white bodysuit (Nova_DT_Armor_light, ghostsuit like Nova's
  NovaReduxFinal_Lights), lights (Nova_DT_New_full, fullbright), hair (Nova_DT_New) and the rifle
  (ghostRifle_unarmed; painted into the suit texture); NovaDarkTemplar = the helmet (nova_DT_glow) and straps, cloth
  and claws (Nova_DT_gloss). A first guess by name put armour on NovaDarkTemplar: scrambled. Tested: correct.
- Default Nova (nova.nod) materials and mesh groups: NovaReduxFinal_gloss 18 (glossenvmap), NovaFace04_hair 14
  (masked), NovaReduxFinal_Lights 12 (ghostsuit, envmap bandtest), NovaReduxFinal_glo 12 (glow), NovaFace04 8
  (normal), NovaReduxFinal_unarmed 2, NovaEyelashes 2 (masked), ghostRifleNEW2_unarmed 1, GhostPistol_unarmed 1;
  all in 3dMaterials.nsa and global.nsa.

## 2026-10-29: Co-op restored (split screen)
- Main menu: "Multiplayer" button under Select Profile (Menu_Main patched: one more button, Options a row down and
  button 2) -> Menu_Multiplayer (28). Competitive is left as it was (no ghostmpcomp.nsc; to be looked at later).
  Cooperative needs state 0x22 in the shell's state history (cShell +0x6EC0 slot 1): only then does
  State_SelectProfileCoop load ghostmpcoop.nsc and the UI profile list (mainProfile_UILoadAll; without it
  mainProfile_pUIProfiles is null and it crashes), so tests must press the button, not write the state.
- Misc\ghostmpcoop.nsc: scripts/make_coop_chronicle.py (ghostsp.nsc with PROFILEID 1, GAMETYPE 1: co-op progress in
  its own slot, so missions start locked).
- Player 2: template Nova_ice with nova_ice.nod (P2CreatePlayer on BeginGame's / CmdNewPlayer's second
  CreatePlayer). Start: P2FindStart in PlacePlayer gives order 1 a copy of the order-0 start 1 m to the side (origin
  when a level has no starts).
- Pads: original bug in gameMultiplayer_UpdateControls / UpdateCameras(Basic) / UpdateRender: the inlined "make
  current" selected the previous player's pad before switching (0x828520), swapping the two players' controls.
  Fixed (pad read from esi). Ports: port 0 = keyboard/mouse (+ XInput pad 0), port 1 = XInput pad 1, or pad 0 when
  player 1 is on keyboard/mouse (port 0 then drops pad 0 while two players are in).
- Rendering: SetCanvas splits 640 into two 320-wide canvases (viewports x 0 / 320). Clear (full-target in GhostPC)
  now clears only the viewport in split screen (it had wiped the left view). Scissor rects are viewport-relative
  (the right view sets 0,0-320,480): GhostPC offsets them by the viewport (sector geometry - ground, canyon walls -
  was clipped away in the right view). renderMain_renderCount advances per tWorld::Draw, so RenderTerrain's
  once-per-frame stamp is per view; Borgo's ground is sector geometry, not cRenderTerrain.
- Open:
- HUD (fixed): each player's view (viewport = its canvas) draws its HUD - radar, health bars and objective box
  during its world render (vuiMiniMap_Render), weapons/ammo in gameMultiplayer_UpdateUI's per-player
  tSequence::UpdateUi - with pre-transformed vertices in full-screen coordinates (crosshair around 320,240), which
  D3D clips to the player's half; only cPlayerInfo::GetScreenPos users were mapped (vui_DrawQuad with flag 1,
  vui_RescaleScreenQuad). GhostPC's D3D layer now maps pre-transformed vertices, and viewports other than a canvas
  (the objective box sets 201,39), into the current player's canvas while two players play and no shell menu / load
  screen is up; GetScreenPos is an identity. Shared parts stay full-screen as in single player: vuiBriefing_Update
  (briefing, dialogue windows) and cMiniMap::DrawHint (objective box). GHOSTPC_LOG_HUDFRAME=<frame> logs one frame's
  2D draws. Cutscene dialogue assumed to be the briefing's windows (not checked in a cutscene yet).
- Whose HUD: HUD code asks gameMultiplayer_GetFirstLocalPlayer (player 1) - health bars, timer bar (searching), crosshair
  state, radar, calldown UI setup, IsValidTargetUI / IsFriendUI - so both halves showed player 1's. Those calls now go
  to the current local player (11 call sites). Mouse look turns player 1's camera only (camera.cpp).
- Co-op deaths: tActor::Kill opens the death menu only when gameMultiplayer_IsSinglePlayerGame; cPlayerControl::
  UpdateDead revives in place on a button press otherwise (competitive). Both checks answer "single player" in co-op
  (chronicle game type 1). Falling: FailMission(HUD_DEATH_FALL) reads "Echo fell to her death." for player 2.
- Shell screens over a split view (pause menu, the load screen between missions) get the whole-screen viewport (they
  were clipped to the right player's canvas).
- Profile names: typed on the PC keyboard into cShellKeyboard (+0xAC text, +0xB4 index, +0xA0 first key, +0xA4
  state): characters, Backspace, Enter (IsValidName -> Done), Esc -> Cancel; the keyboard's pad bindings are off then.
- Co-op mission select: Menu_MultiCoop now sets the menu's +0x2C flag and name MNU_SEL_MISSION like
  Menu_MissionSelect (the list fits and scrolls; it had another layout running off the screen).
- GHOSTPC_AUTOCOOP=<n> picks mission n.
- Zoo in co-op: the level script's player_start warps player 1 (pLeftLocalPlayer, not GuardedPlayer: that is the
  current local player, either one) and player 2 1 m to her side. Other levelscript uses of GuardedPlayer still mean
  "the current local player" in co-op.
- Reticle on both screens: vuiTarget_Draw (from vui_Update, once per player's HUD) keeps its aiming state in globals
  (0x7D69D0 keep-drawing counter, 0x7C3AD8 aiming control type); player 1's aim left them set for player 2's pass.
  Each local player now has its own copy. gameMultiplayer_UpdateRender's pad selection ([edx+0x10]) fixed as well.
- Irradiate / calldown UI missing in co-op: the aim target is one global (0x87B0C8, effectManager_Manager+0x1F80, the
  GUID the aiming player points at), written by every player's cEffectLaserSight::Render (owner +0x5C) and
  tPlayerCalldown::GetTarget (8 calldown vtables, slot 1; player +0x10) and read by the HUD (cUICalldown::DrawTarget,
  reticle, callouts) and UpdateAssistTarget. Player 2's write cleared player 1's target before her calldown UI drew.
  Now one copy per local player: writers use their owner's, the MP loops' per-player sections (their
  stdInput_SetCurrentGamepad calls) switch to the current player's. Not yet confirmed in game.
- Calldown minigame: cUICalldown (one per player's cUISelect) updates outside the players' sections, with player 2's
  gamepad and aim target: her B drove both minigames, player 1's never started. cUICalldown::Update (vtable slot 3)
  now runs with its owner's pad (cPlayerInfo +0x10) and aim copy. GHOSTPC_LOG_CALLDOWN=1 logs each player's state.
- Irradiate in co-op (fixed, tested in 1_2_1): four shared states, all per player now -
  - the aim target 0x87B0C8 and the calldown target 0x87B0CC (effectManager+0x1F80/0x1F84): swapped per player in
    the MP loops' sections; a laser drawn in the other player's view no longer clears its owner's target;
  - ScriptPlayer::SetCalldownEnabled / EnablePlayerWeapon / DisablePlayerWeapon apply to both local players;
  - tPlayerCalldown::Update (slot 4 of all 8 calldown vtables) runs outside the sections and read player 2's pad for
    "B still held": player 1's calldown stopped the frame it started. It runs with its owner's pad now.
  Test: GHOSTPC_WARP / GHOSTPC_WARP2="x y z yaw frame" (warps, camera yaw too), GHOSTPC_SKIP_BRIEFING,
  GHOSTPC_LOG_POS, GHOSTPC_CALLDOWN_TEST; the 1_2_1 gate trigger: warp to -77.79 -145.22 14.85 yaw 26 and walk
  forward; the calldown spot -82.34 -136.41 15.38 yaw 101; aim = one tap of Y (holding it opens the weapon list).
- Echo's irradiate tested too (GHOSTPC_WARP_SWAP=1: player 2 on the second warp's spot; =2 on both). The other
  calldowns (Arclite siege-tank strike, Yamato, Comsat, EMP, Nuke, Identify) share the fixed paths (ActivateCalldown,
  slot 1 GetTarget, slot 4 Update, the two target globals); their strikes (tComsatCalldown / tArcliteCalldown::DoFire,
  tYamatoCalldown::Explode) spawned in GetFirstLocalPlayer's sector, now the calldown's own player's. Not tried in
  game (no reachable level).
- Triggers: ScriptPlayer::GetCurPlayer (player 1) is what ~50 mission scripts compare the triggering actor with
  (cObjectiveUpdate, cLevelChange, cWeaponEnable, cCalldownToggle, cRegionDamage, cM131_*, cM432_BridgeScene, ...).
  While scriptMessage_Triggered / Entered / Exited / Touched / Activated (16 call sites) names a local player,
  GetCurPlayer returns that player. Tested: Echo alone sets off the 1_2_1 gate trigger. Scripts that call
  GetFirstLocalPlayer directly (cM131_ZerglingPop, cM431 marine spawns, cCueRow, cutscene data) still mean player 1.
- Script and cutscene moves of player 1 (cCueRow::WarpPlayerIntoPlace / DoPlayerMove, ScriptThing::SetPosition /
  JumpToThingMatrix -> tThing::JumpToMatrix) take player 2 along, 1 m to her side (gated areas, cutscene starts).
- Competitive arena (paused): levels/arena/make_arena_map.py -> arena.map (8_1_1a_Protoss_Arena; mirror-symmetric about
  y = 0, checked by the script; Aiur AR_* textures; spawns in the north/south rooms via player_start / player2_start
  - player2_start not implemented in levelscript.cpp yet). Still to do: compile script, install, ghostmpcomp.nsc with
  only arena levels, the competitive menu.
- To check: triggers (mission scripts, regions) must accept either player, so neither can softlock the other.
- Ammo between missions: cChronicle::SetPlayerData (health, Psi, every ammo type, powers from the mission's STATS /
  AMMO lines) runs on a level change only after tSequence::ChangeMission (tSequence +0xD4): a new mission starts with
  its chronicle loadout, sub-levels keep what the player has. Same in single player: by design.
- -coop (GhostPC argument, or GHOSTPC_COOP=1): the game starts in co-op (cShell +0x6E34 and the co-op game type set
  until player 2 exists), e.g. "GhostPC.exe -level 1_2_1_Miners_Bunker -coop". Checked every level's start in co-op
  (1_3_1b, 1_2_1, 1_2_1a, 1_2_1b, 2_2_2, 2_2_2a; Borgo 1_3_1a and the zoo earlier): player 2 starts 1 m beside
  player 1 on valid ground in all of them.
- Test input: GHOSTPC_INPUT entries "<frame>:P2 <control>" press player 2's pad (port 1 is reported then).
- Aries (cAIBossSniper::FireSniperProjectile -> FireInstantProjectile): 25 damage a shot, now 80 (push at +0x16).
- Main menu button 0 now reads "Singleplayer" (was MNU_RESUME_CAMPAIGN "Select Profile").
- Co-op profile menus (fixed): Menu_SelProfileMultiCoop (menu 20) is shown for player 1's pick (mainProfile_pPlayer0
  unset; State_LoadProfile sets player 0, then player 1) and again for player 2's (player 1's profile greyed). Both had
  the title MNU_SEL_PROF and "{y} Create New" in both prompt boxes. Now MNU_SEL_PROF_P1 / _P2 (unused strings in
  game.nls), box 3 "{x} Delete" with X -> delete on player 1's pick, empty on player 2's. Delete from co-op not tried
  yet (State_DeleteProfile returns to state history slot 3).
- Co-op missions: IsMissionUnlocked for save slot 1 also asks the other player's profile (a NOVA / NSI profile in
  either seat unlocks all; other profiles keep their own progress). The co-op mission select itself is as designed.
- Test switch GHOSTPC_AUTOCOOP=1: drives the menus by the menu shown (intro, title, Down+A on the main menu, A on
  Multiplayer, profile, missions, difficulty), then A on the finished load screen (state 24) and twice in the level.

## 2026-10-29: Competitive (Protoss arena, Nova vs Echo)
- Arena: levels/arena (make_arena_map.py -> arena.map; compile_arena.bat / .sh with the OpenNOD SDK; install_arena.py
  into game/Levels). Fitted textures (make_arena_map.py, Fitted): pillars 4 repeats per 7 m face from the corner and
  3 up from the floor; corridor walls cut into panels of about 4 m alternating AR_Hall_gld_01 / _02 (the two halves
  of one arch), each texture filling one panel floor to ceiling (Quake standard projection offsets). Ceiling AR_Pattern_HComb_gld (world brushes can only use Materials\* textures, MaterialDirs.txt;
  the 3D\Materials AR_CeilingPanel* are model textures).
- Chronicle: scripts/make_comp_chronicle.py writes Misc/ghostmpcomp.nsc (PROFILEID 2, GAMETYPE 2; one mission
  8_1_ARENA = 8_1_1a_Protoss_Arena, "Protoss Arena Test", loadscreen and menu image 811a_MenuImage) and the strings
  8_1_ARENA, 8_1_OBJ00, MNU_SEL_PLAYERS in the three game.nls copies. IsMissionUnlocked: slot 2 unlocks everything.
- Menus: Competitive (state 0x2B) first rebuilds Menu_Multiplayer as "Number of Players" (2 Players -> 0x2B,
  4 Players greyed: cShellMenuItem +0x28 = 4, skipped by Select*Button; B -> 0x22). State_SelectProfileComp loads the
  chronicle only when the previous state is 0x22, which it still is. Menu_SelProfileMultiComp (an older layout) jumps
  to Menu_SelProfileMultiCoop; Menu_MultiComp gets the co-op list's +0x2C flag and title.
- Kills: tActor::Kill counts them in multiplayer (killer's cPlayerInfo +0x78; +0x7A also for "special" kills;
  projectiles credit their owner, GUID at +0x378). Self-kills no longer count (Kill+0x402/+0x40E). Grenade kills
  count (tested). Spider mines did not: cSpiderMineControl::ExitSpiderMine creates a new "spidermine" thing at the
  mine and kills it, and its death explosion is the killer, unlinked from the rider (control +0x1A0). That creation
  (ExitSpiderMine+0x92) is recorded and Kill's killer check (+0x3C8) maps the thing to the rider; a player death that
  credits nobody is logged ("no kill credited", with the killer's type). Mine kills not tested yet.
- Match (competitive.cpp): clock from the level load, paused while a shell menu is up; time limit from the settings
  window ([multiplayer] MatchMinutes, default 10). Timer top centre (red under 30 s), scoreboard while Tab is held
  or Back (either pad, Backspace; in a match Back is taken from the game, whose controls screen it opened), names from
  the profiles (cProfile +8). Texts are cUiText objects
  (0x40 bytes, ctor(text, x, y, ARGB, buffer size)) drawn after vui_Update's RenderAll with a full-screen viewport
  (d3d_core SetFullScreen2D). At the limit: victory screen (or MATCH OVER on a draw), controls frozen
  (gameMultiplayer_UpdateControls skipped), 60 s or Space / A, then cShell::MenuOn + state 0x4E = State_Quit (the pause
  menu's Quit; UpdateState runs only with the shell on, and State_Quit needs its menu object): title screen.
- Menu image: UI/Materials/811a_MenuImage(L).dds from levels/arena/menu_shot.png (levels/zoo/make_menu_images.py
  --name 811a_MenuImage, crop 400 160 1600 760). The mission select's info panel (cShellMenuItem::RenderText "!"
  command -> RenderMissionDesc) draws only the name unless the item is selected (+0x30); in a one-mission list
  select-next wraps onto the same item and clears it, so the panel was empty: competitive.cpp keeps it selected.
  Test switches: GHOSTPC_AUTOCOMP=hold (stop at the mission list), GHOSTPC_LOG_MISSIONDESC.
- Pause menu in a match: Start on either pad opens it (Continue ... Restart, Quit), full screen (tested).
- Respawns: random "respawn" point, not within 8 m of the other player, nor the point she took in the last 5 s.
- Tested (scripted): menus into the arena, timer, scoreboard, victory screen, return to the title screen.
  Not tested: an actual kill (scripted R Trigger did not fire in the arena) and the competitive respawn.
- Test switches: GHOSTPC_AUTOCOMP=1 (menus into a competitive game), GHOSTPC_MATCH_SECONDS=<n>, GHOSTPC_SHOW_SCORE=1
  (scoreboard always), GHOSTPC_WARP2_ONLY=1 (the second debug warp moves player 2 alone).

## 2026-10-29: Player vehicles in the build (survey)
- tThing::PreFinalize gives a player-type thing (type 7, actor flags & 0x200) a control by substring of its template
  name (tThing +0xC4): "goliath" cGoliathControl, "arclite" cTankControl, "flameturret" cFlameGunControl, "turret"
  cGunControl, "refineryarm" cRefineryArmControl, "spidermine_player" / "firefly_player" / "mantis_player" (the last
  two derive from cSpiderMineControl), "vulture_player" cVultureControl; Vulture / Goliath / tank share
  cVehicleControl (gears, RPM, throttle, dismount position).
- Templates (Global.not): Vulture_player (novaVulture.nod, 80 HP), Goliath_player (Goliath.nod, novaGoliath.nms,
  275 HP), Arclite_player = the Siege Tank (siegeTank_black, novaSiegeTank.nms, 900 HP; siegeTankCD is the Arclite
  calldown, not this), Turret_player (gunTurret_nova.nod, Gauss, 350 HP), FlameTurret_player (flameTurret_nova.nod),
  RefineryArm_player (CH_craterCapArm.nod, the Refinery's crane), spiderMine_player, Firefly_player
  (itemFirefly.nod), Mantis_player (no model of its own: the spider mine's). Every model and motion set is present.
- Remote units are inventory items (tPlayerItemSpiderMine / Firefly / Mantis; pickups powSpiderMine, powFireFly,
  powMantis); no active mission's INVENTORY grants them, the arena's spider mine pickups do.
- Placed in this build's levels: only Turret_player (1_2_1a_Miners_Bunker). The siege tanks of 1_2_1a / 1_2_1b,
  the Zoo's Goliaths, Vultures, Hauler boss and siege tank are AI units or props. The levels that used the other
  vehicles are not in the build; tutorials 026_TurretVehicles, 027_VultureVehicle, 028_GoliathVehicle and
  029_SiegeTankVehicle (.nlt) are.

## 2026-11-04: OpenNOD SDK 0.6.8 skies, Zoo pickups, vehicle test map
- SDK 0.6.8 (levels/sdk, gitignored; compile scripts default to %~dp0..\sdk): MAPtoNSD worldspawn keys nod_sky_model
  (.nod name, trailer +128), nod_sky_animation (.nad, +160), nod_sky_unmasked (header word at offset 72: 2 = sky over
  the whole viewport, 0 = only through sky-mask surfaces). gameScene_Load strips the extension before
  renderModel_LoadInstance; an empty or missing animation is skipped. Every original outdoor level (1_2_1*, 1_3_1*,
  menu) uses 2 and has no ceiling over open areas; 2_2_2* use 0 (one "sky" surface; radiant.nsa's "sky" material is
  editor-only: surfaceparm sky, nonsolid). MAPtoNIL now also takes archived (.nsa) materials by name.
- Enemy Zoo: nod_sky_model sky_Robota_02.nod + unmasked, the textured ceiling over the big rooms removed (install_zoo
  no longer renames a placeholder or writes sky names). Tested: the Robota sky shows above the open rooms. Spawn room:
  powSpiderMine, powFireFly, powMantis (tested: collected, 4 of each; a remote unit launched with its own view).
  Flyers are no longer capped by a ceiling (not yet watched in a long run).
- Vehicle test map (levels/vehicles, level 9_2_1a_Vehicle_Test, mission 9_2_VEHICLES "Vehicle Test map" after the Zoo
  in ghostsp/ghostmpcoop.nsc via scripts/add_vehicle_mission.py; unlocked for every profile like the Zoo; 7_1 is the
  original game's cut missions). Grass field inside rock walls, a stadium asphalt track (convex prisms) around a solid
  block, Protoss spawn room at the origin, the Robota sky. A Vulture_player on the track: tested, B mounts it (first
  person cockpit, Exit on B) and it drives.
- MAPtoNIL CSG lessons: corners of non-axis-aligned brushes must be exact binary values (snapped to 1/64 m); decimal
  rounding gave "CSG vertices merge at binary32 precision" where curved pieces met (the weld tolerance is
  ~128 * DBL_EPSILON * coordinate). Lit compiles with nod_light_spacing below 128 broke 160 m walls into stray
  triangles up to 60 m outside the level (NILtoMAP shows them); 128 is clean.
- Player vehicles placed by the SDK got cHideControl ("Hide", cover camera, no driving): tThing::PreFinalize matches
  the thing's name with strstr against lowercase names, original levels place lowercase ("turret_player"), the SDK
  writes Global.not's spelling. vehicles.cpp makes the nine checks case-insensitive; now Vulture_player ->
  cVultureControl, Goliath_player -> cGoliathControl, Turret_player -> cGunControl, FlameTurret_player ->
  cFlameGunControl (RefineryArm_player -> cRefineryArmControl: a remotely worked crane, not for the map).
- Vulture controls (stdInput slots 0..7 = White, X, Y, Black, A, B, LT, RT): A accelerates (the NSI throttle goes
  negative = forward, ~31 m/s), X brakes (no reverse), B dismounts, RT fires, White cycles 3 cameras, Black looks
  back; the stick only steers. input.cpp maps the left stick forward / back (W / S) to A / X while riding. Tested:
  W drives it ~78 m up the track. Debug: GHOSTPC_MOUNT=<frame> (mount the nearest Vulture), GHOSTPC_LOG_VEHICLE.
- No vehicle sound: the build has no .nag foley files at all (playerVulture.nag) and no playerVulture sound bank.
- Vulture sounds (custom): a foley set is the bank Sounds\<set>.xsb (tFoleySet::APILoad), each foley mode a cue named
  gameFoley_GetModeName(mode). The Vulture plays wear (mount), anim_idle (mount), lwalk_solid (engine, with a handle:
  SetPitch every frame, ~0.7 at rest up to ~2.4, Stop below 0.01 m/s and on dismount), remove (dismount), hitsolid
  (collisions); its gun uses projTerrSgeTnk, its mines terrSpiderMine (both present). customsound.cpp now takes a
  missing bank's set name from the object's template (modifier 10 foley, through basedon) and loops custom sounds
  played with a handle (GhostPC handles 0xC57xxxxx; cGameMixer::SetPitch / Stop detoured for them). Tested with
  placeholder tones: wear, engine loop with rising pitch, hitsolid, stop and restart.
- Siege Tank (Arclite_player -> cTankControl) works as placed: drives with the stick, X transforms into siege mode,
  RT fires artillery shells; "Exit" B. Goliath (cGoliathControl): walks with the stick, RT fires its chainguns.
  Tested via GHOSTPC_MOUNT with GHOSTPC_MOUNT_NAME=Arclite / Goliath (GHOSTPC_NOMOUNT, GHOSTPC_DROP: debug).
- Idle player Goliath: not simulated, it stays where the level puts it (+0.2 m); placed 0.3 m lower so its feet touch
  the ground (vehicle map).
- Goliath chaingun sound: StandControlsUpdate plays foley "swing" (mode 5) once when RT goes down and holds the handle
  (+0x1CC) until release. The cue is a one-shot (global.xwb wave 0xEF, 0.94 s: ~270 ms of full fire, then a tail), so
  held fire went quiet after the first burst. The Goliath bosses replay that wave with every shot of each gun (Zoo log).
  vehicles.cpp clears the handle one frame after each play, so every chaingun shot (every 50 ms) plays it again, and
  stops each instance after 950 ms; the mixer keeps instances until Stop, so cGameMixer::FindInstance cannot tell that
  one has ended.
- Vulture bullets (proj_vultureGun, foley projTerrSgeTnk, shared with proj_turretmine) play foley mode 0 "create" on
  creation; the bank's only cue is createnoblock (mode 320), the quiet "pewpew". Sounds\custom\projTerrSgeTnk\create.wav
  replaces it (for the turret mine too).
- Remote units' mouse look: cSpiderMineControl (Firefly / Mantis derive from it) turns the unit, not the camera, with
  the right stick: UpdateControls reads stdInput_GetAnalogJoyStick(1, 0) (right X) and yaws the mine's look direction
  at 180 deg/s (mathVector_Yaw, LookDir); UpdateCamera is a chase camera whose pitch comes from GetCameraPitch. Direct
  mouse look (camera.cpp) gave the stick nothing, so the mine could not turn. MouseDirect() limits direct mouse look
  to Nova herself (player control vtable cPlayerControl); in a vehicle or remote unit the mouse is the right stick.
- Remote units' design (simControlSpecial.obj; game.nls 1_3_1b_SPIDERMINEHINT): Spider Mine walks (left stick),
  turns (right X), RT detonates, B burrows ("landmine mode": ExitSpiderMine creates a "spidermine" thing, a cAIMine,
  where it stood); quitting leaves a pickup. Firefly: X = DoDistraction (foley, ffDistract effect,
  aiAwareness_Critical: AI within 30 m investigate; also on dismount), B exits. Its DoSilentKill (ThingsInSphere 3 m,
  the closest live actor: Kill 100 credited to the Firefly + ApplyBlastPhysicsObjects) had no caller and no button.
  Mantis: RT on the ground = EnterHoverMode (narrower FOV, hover chase camera); hovering: RT up, LT down, left stick
  moves, B exits, lands near the ground; a spy camera, no attack. ePlayerItems: canister 0, smokegrenade 1,
  stickygrenade 2, hellfire 3, spidermine 4, firefly 5, mantis 6 (cMission::InventoryTokensToItemCount slots);
  tPlayer +0x4C4[item] = the item objects; ActivateCurrentPlayerItem needs Nova's throw, the launch itself is
  tPlayerItemSpiderMine::Fire(matrix) (Firefly / Mantis items too).
- vehicles.cpp: Firefly RT = DoSilentKill (cFireflyControl vtable slot 68 UpdateInput wrapped; not while Nova's body
  is within 3.5 m, 1 s apart). Tested in the Zoo: the Firefly flown to the mechanic, RT -> Kill credited to it.
  Burrowed mine pickup: when a Spider Mine ride ends, a new "spidermine" thing within 2 m of the mine is recorded;
  Nova walking over it (1.2 m, after 2 s) removes it and adds a spider mine (tested: 4 -> 5). A mine burrowed next to
  Nova's own body (about 2 m) goes off at once (cAIMine::ASStartChase targets her) and kills her: original behaviour;
  one burrowed farther away lets her walk up to it. Debug: GHOSTPC_ITEM="<frame> <item>" launches a remote unit.
- Custom vehicles (survey): a vehicle = a template (model, motionset, foley, effects, health, armor, PHYSICS_MASS,
  ACTOR_FLAGS 1536 = player-type) + the control tThing::PreFinalize picks by a substring of its name ("vulture_player",
  "goliath", "arclite", "turret", "spidermine_player", ...), so a new template named e.g. "Raider_vulture_player"
  gets cVultureControl. Templates: gameTemplate_Startup opens "global" (Misc/Templates/Global.not); gameScene_Load
  also opens every template file a level's .nsd lists (32-byte names), so a level can bring its own .not
  (NOTEditor creates templates and archives) without touching Global.not. Driving physics is hard-coded per control
  (cNSIVulture::Init: SetupWheels / Steering / Engine / Transmission / Brake / Aerodynamics constants; cNSITank::InitTank
  for the tank); only the suspension reads the model: wheel positions from markers of type 0x22 (SetupSuspension ->
  GetMarkerPos). A new model therefore needs those markers (and the Goliath a matching skeleton + motion set). The SDK
  has no importer from common 3D formats to .nod (NODtoNMB bundles existing NOD/NOC models).
- Reaver (for a custom player vehicle): template reaver_prop (_ProtossProps: reaver.nod, reaver_prop.nms, radius 3.8,
  height 5, editor size 12 x 17 x 11 m, DEMOANIMS AI). Motion set: stand, walk (= run), turnleft, turnright,
  firestand (= stand), recoilstand (Reaver_attack, no loop). .nad format (tRenderKeyTrack::LoadData): u32 version 4,
  u32 track count, u32 flags, f32 length (frames, 30/s); per track u32 key count, u32 bone, u32 0, then 52-byte keys
  (time + 12 floats; tracks come in rotation / translation pairs per bone; GetRootTranslation = bone 1's last key).
  Reaver_walk: 39 frames (1.3 s), no root motion (bone 1 still): an in-place inchworm crawl, body segments sliding
  about 1.5-1.8 m per cycle -> about 1.2-1.4 m/s.
- Scarab: no model. proj_scarab is a copy of proj_protossTurret (tossTurretProj glow, exp_protTurretProj, 50 m/s,
  5 s, PROJECT_FLAGS 15). Projectile flag 0x4000000 = homing (PrePhysicsUpdateProjectile steers to a target GUID;
  the Goliath / turret missiles use it); 0x1000 is set on the Lurker / Hauler / hydralisk projectiles (along the
  ground, not yet confirmed). Placements name their template file (nod_template_archive), so a level can use a
  custom .not.
- Vehicle test map: the Siege Tank moved to the east straight with the others (VULTURE x, y +14).
- Reaver vehicle (in progress): GhostPC template file game/Misc/Templates/ghostpc.not (scripts/make_ghostpc_templates.py;
  records need the editor-data block, u32 1 + one zero byte, or MAPtoNSD rejects the file). reaver_vehicle_player =
  Arclite_player + reaver.nod / reaver.nms, MODELSCALE 0.5, 600 HP; PreFinalize's "arclite" check matches it too
  (ControlNameMatch) so it gets cTankControl, whose UpdateControls (vtable slot 10) is wrapped: X / RT hidden from the
  tank code (no siege, no shells), speed held to 1.3 m/s, motions walk 1 / stand 3 / turnleft 132 / turnright 133 /
  recoilstand 48 (gameMotion_apNames indices), RT = Scarab every 2 s. Map placements must share one archive context:
  nod_template_archives "ghostpc.not" on every placement (Global is implicit); the scene then lists ghostpc.not and
  the game opens it with the level (GhostPC also opens it at startup: gameTemplate_OpenFile("ghostpc")).
- Scarab: proj_reaverScarab = proj_scarab + PROJECT_FLAGS 0x400110F (the Goliath missile's; 0x100: the shooter sets
  the homing target, projectile +0x37C = target GUID, then tProjectile::StartWithAccelerate), 200 damage, 12 m/s,
  EFFECT2 scarabTrail. Fired with simCombat_FireTemplateProjectile(tmpl, matrix, owner, owner GUID, 0, 0, 0, 1).
  Effects: game/Misc/ghostpc.nfx, appended to game.nfx when the game opens it (xapi.cpp serves
  <save>\ghostpc_game.nfx). Tested (GHOSTPC_SCARAB=<frame> fires from Nova): the Zoo's zealot hit for 200 twice and
  killed; dead actors carry +0x360 bit 0x10000000. Hibernating units (_hib) are not hit (homing circles them).
- Zoo: novaVulture_cut replaced by firebat_elite. Vehicle map: test enemies on the west straight.
- Reaver on the tank code (tested, user confirmed): the tank's rigid body is a convex hull of <model>.noc
  (simPhysics_SetupTankRB +0xCA renderModel_Load) and its suspension reads wheel markers 0x22 #1-4 (tank
  SetupSuspension = InitTank - 0x17B0; the Vulture has a function of the same name). The Reaver has neither: the hull
  load falls back to siegeTank_black.noc and missing wheel markers get footprint corners (+-3, +-4, 0.6 model units,
  through the matrix GetMarkerPos was given). The tank code's 15 PlayMotion calls skip the Reaver (they reset it to
  stand every frame, so the walk never advanced). The X prompt is the control's action byte +0x16 (read by vtable
  slot 0, GetStealthActionButton); action 25 = HUD_ACTION_TANKMODE is cleared for the Reaver. Ghost.exe is relocated
  at runtime: patch sites are found from symbols, never by absolute address.
- Reaver / Scarab sounds: no Reaver foley bank in the build (reaver_prop uses enemyDragoon; game.nls only names it,
  LBENTRY_PROTOSSREAVER). The Scarab's explosion is: Sounds/expPtssScarab.xsb, one cue createnoblock = protoss.xwb
  wave 0xAA (2.2 s; neighbours 0xA9 photon cannon, 0xAB Dragoon explosions), used by no template. GhostPC's
  exp_reaverScarab (exp_protTurretProj + foley expPtssScarab) is the Scarab's EXPLOSION; an explosion only plays
  create (mode 0, tThing::Finalize) and createnoblock (320) only plays from tThing::UnHibernate, so customsound.cpp
  plays createnoblock for expPtssScarab's create (user: heard in game). Unreferenced-looking protoss waves below 0xEE
  (plus 0xA9-0xAB) were decoded to test/protoss_waves/ for listening (the reference scan misses variation tables, so
  the "unused" list is not reliable above 0xEE, which looks like voice lines).
- Reaver sounds: foley ptssDragoon (ghostpc.not). The tank code's engine cues (moving, mode 4, every frame while
  driving; lwalk_solid 23 with a handle as the engine loop) are dropped for the Reaver (customsound.cpp); vehicles.cpp
  plays the Dragoon's footsteps instead, lwalk_solid 23 / rwalk_solid 32 alternating every 0.65 s (two per 1.3 s walk
  cycle) while it crawls or turns. Tested: steps at frames 401, 421, 441, ... while driving.
- Reaver at native size (no MODELSCALE; radius 6, height 5), placed on the open grass east of the track (-26, -40):
  at full size next to the other vehicles its hull overlapped the Goliath and it could not move. Hull: the borrowed
  tank hull (x +-3.05, y -3.4..5.0, z 1.16..4.0) scaled 2 x 2 x 1 (scaling its height lifted it off its wheels: a 4 m
  jump at setup); wheel points +-3 / +-4.2 at 0.6 (wider ones made it bounce and stall); it rests at about 0.3-0.4 m.
- Mounting a vehicle: cPlayerControl::UpdateActionButton finds the closest thing with thingflags 4 in a 1.5 m sphere
  around Nova's feet, then needs her within 1.5 m of one of its entry markers (type 0x0F, 0 / 1, GetMarkerMatrix:
  +0x10 facing, +0x30 position) and facing it; then SetActivateTarget + prompt byte +0x1A = 6 ("Mount", 0x17 for
  control type 0xA); cPlayerControl::Activate (B) checks thingflags 4, type 7, +0x360 0x200, not dead, and calls
  tPlayer::MountVehicle. The Reaver's hull underside (1.6 m) is out of the sphere's reach: UpdateActionButton is
  detoured and, with nothing else to do, Nova within 1.5 m of the Reaver's hull (any side) and facing it gets it as
  her target with "Mount" (user: one step close shows it). GetMarkerMatrix is detoured too (missing 0x0F markers of a
  Reaver: its sides, 7 m out). GHOSTPC_WARP's yaw does not turn Nova's facing (+0x90) until she moves.
- Reaver camera: tSimCamera::BeginChase copies the tank's preset (avTankCameraPosOffset / LookPosOffset: (0,-10,6) /
  (0,3,4), (0,-10,8) / (0,3,5), (0,0,4.5) / (0,8,4.5)) into the camera (+0x1B8 position target); while driving a
  Reaver a tank preset found there is replaced (SetChaseCameraPosOffset / SetChaseLookPosOffset, eased): (0,-24,14) /
  (0,6,6), (0,-30,19) / (0,6,7), (0,7,7.5) / (0,18,6.5).
- Reaver size settled at MODELSCALE 0.75 (user: native was too tall; 1.5 x the first 0.5): about 9 x 12.75 x 8.5 m,
  radius 4.5, height 3.75. vehicles.cpp REAVER_SCALE scales the hull (tank hull x/y * 2 * scale), the mount zone, the
  side entry points and the camera presets (native values * scale). Tested: rests at 0.03 m, about 0.3 m while
  driving, crawls about 1.6 m/s.
- Scarab aiming (user: they dodged enemies): the game's homing (0x100, target +0x37C) turns too slowly at 12 m/s, so
  Scarabs overshot moving units and circled them; and the target filter treated actor flag 0x20000 as dead, though
  live zerglings and hydralisks carry it (only 0x10000000 marks a dead actor), so only some enemies were ever chosen.
  Now: the target is the live AI unit closest to straight ahead (60 degree cone, 40 m, nearer ones winning
  near-ties), the Scarab launches from the Reaver's head, and vehicles.cpp (SteerScarabs) points each Scarab at its
  target 0.5 m above its feet every frame (SetVelocity 12 m/s, LookDir3d). Tested on charging zerglings, a hydralisk
  and the elite Firebat: direct 200 hits on each, one zergling killed by the splash.
- Scarab damage 1200 (PROJECT_DAMAGE, tested: a Zoo zealot dies to one) and 3 s between shots (REAVER_RELOAD).
- Reaver engine: the Dragoon has no engine cue; its idle hum is ptssDragoon special1 (mode 152), played by keyframe
  events of its idle animation in pairs, 13-15 frames then about 24 apart (Zoo log). The Reaver plays it on that
  rhythm (0.45 s / 0.8 s) the whole time it is driven; the footsteps are gone and the tank's engine cues stay dropped.
  Tested: special1 at frames 251, 265, 290, 304, 329, 343.
- Reaver driving (user: it slid sideways with the mouse, should drive straight and turn slowly): cTankControl's
  DriveControlsUpdate drives toward the left stick's direction relative to the camera (stick +0x54 / +0x58 through
  the camera axes, mathTrig_ArcTan4 against the hull's heading), so the mouse steered it. For the Reaver the stick is
  hidden from the tank code and GhostPC drives it: forward / back = velocity along its heading at REAVER_SPEED 0.9 m/s
  (its walk at 0.75 scale), left / right = its matrix turned by 0.6 rad/s * frame time and set with
  tThing::SetPhysMatrix (tThing::SetRotVel had no effect: the raycast wheels and the angular damper hold it).
  Tested: straight crawl about 0.85 m/s, turning about 34 deg/s with the turn animation.
- Exit crash: the tank's update can dismount (B) inside the wrapped UpdateControls; the code after it then read the
  control's cleared camera (cControl::GetCamera on null). It now stops unless a local player still drives the Reaver.
- Reaver camera presets lowered (native (0,-18,9)/(0,6,4), (0,-24,11)/(0,6,4.5), times 0.75) to stay under the field's
  12 m walls. Target marker: vuiTarget_SetTarget(Reaver, ReaverTarget) every frame while driven, on the unit the next
  Scarab goes for (the tank's drive code sets it to its own target).
- The stray rings the user saw ("points down under the map") are the shell trajectory arc (vuiPlayer_pGrenadeArc,
  cGrenadeArc::Set), drawn by cTankControl::UIRenderCallback (vtable slot 26) from model marker type 2; the Reaver's
  points down. Slot 26 skips the Reaver (TankUIRender).
- Reaver jitter (user: jittery, also standing still). Logged every frame with GHOSTPC_LOG_REAVERPHYS:
  - Driving with a velocity set every frame, the raycast wheels braked it. It lurched 0 to 0.12 m per frame (sometimes backwards) and bobbed.
  - Standing, the borrowed suspension rocked it about 0.1 m back and forth every half second, endlessly.
  - Fix: horizontal velocity is zeroed, and moving and turning go through tThing::SetPhysMatrix every frame, standing still too. Parked Reavers get their own matrix every frame (HoldParkedReavers).
  - Tested: a steady 0.030 m per frame, height constant at 0.288 m (0.085 m parked), turning 1.13 deg per frame.
- Blocked: when the physics pushed it back (forward progress under 30 % of the step), it stops pushing for 0.5 s. Pushing on scraped it along the wall and tilted it about 6 degrees.
- Vulture as a base (considered, archived tank version in archive/reaver_tank_base/):
  - cNSIVulture is also a Havok raycast vehicle: 2 wheels with hard-coded hardpoints (0, 1.5, 0.4) / (0, -1.9, 0.4); markers 0x22 #0 / #1 only gate it.
  - Suspension: length 1 m, strength 60, damping 3. That is the hover.
  - Also: an upright constraint, its own doSimulationStep forces (applyLinearImpulse) and stick-X steering (it cannot turn in place).
  - Its hull is <model>.noc too (simPhysics_SetupVultureRB through cShapeRepository).
  - It would need the same hull, marker and motion workarounds plus suppressing the hover. The jitter came from GhostPC's driving, not the tank base.
- Reaver hull (user: wall collision at the wrong spot): the scaled tank hull ran from 5.1 m behind its centre to 7.5 m ahead (head at 6.6 m, tail at 6.2 m).
  - Now a prism over reaver.nod's vertex outline, built with the game's simPhysics_CreateConvexHull(points, count) at SetupTankRB +0xD7.
  - Native outline: (+-2.7, 8.7) (+-5.3, 6) (+-6.2, 2) (+-5.6, -2) (+-1.2, -8.3), times REAVER_SCALE; z 1.16 to 4 m.
  - Mount zone: 1.5 m around that outline's box.
  - Tested: driven into the field's east wall (x -14), it stops with its centre at -20.5 (head on the wall), no tilt.
- Reaver speed (user: a little too slow): REAVER_SPEED 1.1 m/s and its walk at 36 frames/s instead of 30. tObject::PlayMotion's third argument is the playback rate: the track length is divided by it.
- Reaver aim path (user: fix the underground crosshair).
  - cGrenadeArc::Set takes a matrix, a velocity in its frame (the tank's (0, 50, 0)) and display settings.
  - Render steps it until it hits something, drawing rings along it and a circle where it lands.
  - For the Reaver, cTankControl::UIRenderCallback (slot 26) now sets it to the Scarab's path: from 2.5 m before its head (inside its own model the path ended at once; GHOSTPC_ARC_AHEAD tunes it) straight at ReaverTarget's unit +0.5 m, or the ground 20 m ahead.
  - It is aimed up by the drop over its 50 m/s flight.
  - Not yet seen in game after the start-point change.
- Reaver camera raised (native (0,-18,11.5) / (0,12,2), (0,-24,13) / (0,12,2.5), times 0.75): under the 12 m walls, above its 8.5 m back, so the ground ahead shows.
- Reaver height (user: floating): wheel points at z 0.85 model units (was 0.6). The body rests about 1 m lower per unit. Tested: 0.04 m driven, -0.06 m parked (0.29 / 0.09 before).
- Reaver sounds (user's picks): a loop of protoss.xwb wave 0xC6 (6.24 s, loop region = whole wave, no cue references it) the whole time it is driven, with the Dragoon's hum (ptssDragoon special1) on top while it moves.
  - The loop is game/Sounds/custom/ghostpc/reaver_idle.wav, played by customsound.cpp CustomLoopStart / CustomLoopStop.
  - It stops on dismount or level change. Tested: one loop through a drive, stopped at B.
- scripts/xwbwaves.py: list / decode a wave bank (unused waves, one wave, an .xsb's waves; Xbox ADPCM decoder).
  - Unreferenced waves dumped for listening (test/, gitignored): terran 661 of 894 (test/terran_waves; from about 0xF1 mostly 1-2 s clips, likely voice lines), zerg 137 of 352 (test/zerg_waves).
  - The scan misses variation tables, so "unreferenced" is a candidate list.
- Protoss console: AR_hackConsole_floor (Global.not, _ProtossProps, AR_hackConsole_floor.nod: materials AR_hack_Console_Floor + AR_Glowpanel_03, about 0.5 x 0.7 x 1.3 m).
  - It is a plain prop: thingflags 2244, no HACK_CONSOLE description like the Terran _console.
  - Also AR_Protoss_Panel_01..04 (DESCRIPTIONID NEXUS, about 3.7 x 1.8 x 2.9 m) and AR_ProtossCircuit_01.
  - No level in the build places any of them.
- Reaver worked from a console (user: it is a robot).
  - The console: AR_hackConsole_floor, placed 13 m behind it on the vehicle map (-30, -53).
  - Using it: UpdateActionButton is wrapped. Nova within 1.5 m of the console, facing it, gets the nearest live Reaver (150 m) as her activate target with prompt 7 (GetActionText: 5 and 7 = HUD_ACTION_ACTIVATE, 5 with the thing's own label from +0x108, 6 MOUNT, 23 HIDE, 24 EXIT, 25 TANKMODE). B mounts it (MountVehicle). The Reaver itself offers nothing now.
  - cTankControl::Mount / Dismount (vtable 53 / 54) copy the entry marker (0x0F) matrix into the rider. While a Reaver is worked from a console, its entry marker (GetMarkerMatrix detour) is Nova's own matrix from mount time, so the tank code leaves her there and puts her back there. The wrappers unhide her (HidePlayer(0, 0)), play stand and clear her control's wait (+0x16C).
  - B: the tank's exit needs FindDismountPos to find room beside the vehicle (it failed with Nova 47 m away). B is hidden from the tank code; the Reaver calls tPlayer::DismountVehicle(reaver, 0, 1, 0) itself. Tested (scripted mount from spawn): Nova stays at the spawn while the Reaver drives; B puts her back there, and she walks off.
- Instant camera, as the Spider Mine (cSpiderMineControl::Mount: keep camera +0x1B8 / +0x1D0, BeginChase(its offsets), UpdateChase(1, 5) = at once; Dismount: BeginThirdPerson(1, 0, 0) + BeginChase(kept offsets)). The Reaver does the same with its chase preset; the tank's camera flew over.
- Scarab larger: reaverScarabGlow (ghostpc.nfx, tossTurretProj's partcylinder at 2.7 instead of 1.5) and the trail at 2.2 -> 0.35 (was 1.2 -> 0.2). The vehicle map preloads it (out_effects.nlfx).
- Camera to the Reaver (user: it still flew there slowly). tSimCamera::UpdateChase's first argument starts a lerp (StartLerp; second argument = speed in m/s; +0x1A4 = 1 while it runs; the lerped point is +0x240). It does not mean "at once":
  - The tank's StartState lerps at 10, the Spider Mine's Mount at 5 (it starts next to Nova).
  - From the console the lerp went through the ground, which is where the rings were seen from below.
  - Now the Reaver's mount cancels it (+0x1A4 = 0) and calls UpdateChase(0, ...). Logged: no camera field eases after mount.
  - Debug: GHOSTPC_LOG_CAM logs the camera fields that change after a Reaver mount.
- Every Reaver hook leaves other vehicles to the original code (IsReaver checks first), so the Siege Tank is unchanged.
- Console prompt (user: it said "Door").
  - The game's own search picked the console itself (thingflags 2244 has the vehicle bit 4; activate target +0x238 = its GUID). Its description falls back to _PROP = "Door".
  - Now a Reaver console picked by the game is replaced with the nearest Reaver.
  - The prompt is action 5 with the control's label +0x1B = "HACK_CONSOLE": DrawActionWindow shows action 5's label through gameString_Find, here "Access Terminal".
  - cPlayerControl::Activate acts by the prompt (GetActionButton): 5 = use a switch (LookAt, press motion), 6 = MountVehicle. WalkUpdate calls it right after UpdateActionButton, so on the frame B goes down the prompt is 6.
  - Tested: Nova warped beside the console, B -> Reaver worked from there; B -> back at the same spot.
- Console moved into the spawn room against its east wall (map: (sx1 - 0.7, 0), yaw 90; user's choice). Needs a map recompile.
- Console use motion: on B at a Reaver console, Nova turns to it (tThing::LookAt) and plays mode 0x87 (nova_use.nad, as cPlayerControl::Activate does for a switch: 2466 ms); the Reaver is mounted when it ends (ConsoleUseUpdate); the prompt is cleared on that frame so Activate does nothing.
  - At level start the B slot's "pressed on frame" stamp and the frame counter both read 0: B also has to be held (value > 0.5), or Nova "used" the console at load.
- Dragoon (template dragoon in Global.not): dragoon.nod, Dragoon.nms, foley ptssDragoon, radius 1.6, height 2.6, 100 health + ACTOR_SHIELD 80, armor 0.4/0.6/0.1/0.6/0.6/0.8, walk 2.4, run 5.6 m/s, PHYSICS_MASS 500, AI PROTOSSDRAGOON.
  - cAIProtossDragoon: AttackRange 12 m. StartAttackRanged plays attack1 (mode 15) and fires on its keyframe tag 4: FireProjectile("proj_dragoon", accuracy 0.9 = up to +-4.5 deg spread, marker type 2).
  - proj_dragoon: 50 m/s, 5 s, PROJECT_FLAGS 15, no PROJECT_DAMAGE of its own, explosion exp_dragoonProj (foley expPtssPhtnCnn, effect dragoonProjExp).
  - Dragoon.nms: stand, walk = run (Dragoon_Walk_N), turnleft / turnright, strafe_n/w/e/s, attack1, hurt, deathslow, and leg IK tracks special1-8 (front / back legs), the Goliath's convention (special1-4).
- cGoliathControl (the Dragoon's base) is a walker, not a raycast vehicle.
  - StandControlsUpdate: the left stick relative to the camera; it turns toward it and walks once it faces within ~60 degrees (dot > 0.5); speed +0x268 ramps (10 m/s^2) to stick x 8 m/s; SetVelocity along it; walk played at +0x268 x 5 frames/s.
  - RT: FireChaingun(i, lock-on target, f) every 50 ms; FireRocket.
  - Leg IK: InitLegIK finds special1..4; UpdateAllLegIK uses markers 0x13 / 0x15 (1, 2) and foot joints 9 / 5.
- Dragoon vehicle (dragoon_vehicle_player, ghostpc.not: basedon Goliath_player + the Dragoon's model, motions, foley, size, health, shields, armor; ControlNameMatch "goliath"):
  - The Goliath code's PlayMotion calls (6, Killed .. UpdateControls) go through GoliathPlayMotion: walk / run x 2.5 frames/s (30 frames/s at its 2.4 m/s walk), recoilstand skipped, nothing while attack1 plays.
  - FireChaingun (2 calls) -> DragoonFire: attack1 + proj_dragoon from marker 2 at the lock-on target (+1 m) or ahead, once per attack motion. FireRocket (2 calls) does nothing.
  - UpdateControls (vtable 10): speed held to 5.6 m/s.
  - Missing entry markers: its sides, 2.4 m out (GetMarkerMatrix detour).
  - Map: south of the Goliath (VULTURE x + 3, -22); preloads dragoonProj, dragoonProjExp, dragoonChrg.
  - Tested so far: hooks install, the real Goliath still walks. The Dragoon itself needs the map compile.
- Dragoon tested (scripted mount, map compiled):
  - Mounts through its supplied side entry point.
  - Walks about 5.5 m/s.
  - RT fires proj_dragoon from its own fire marker (type 2): attack1 = 1966 ms, so one shot per ~2 s, as the AI's attack cycle.
  - A second RT inside that window waits.
  - Not yet seen: leg IK, the shot hitting a unit, shields.
- Dragoon footsteps (user: one leg per cycle): Dragoon_Walk_N (44 frames) plays lwalk_solid and rwalk_solid together once per cycle (keyframe events). For the vehicle those are dropped (customsound.cpp, unless g_ghostpcStep) and vehicles.cpp plays four per cycle, left / right alternating, at its ground speed (walk at speed x 12.5 frames/s).
- Dragoon aiming:
  - The target is the live AI unit nearest the middle of the view: camera +0x8 = position, +0x74 = look point; within 12 degrees and 50 m. It is shown with vuiTarget_SetTarget.
  - The shot goes to its tActor::GetTargetPos (the body), led for 50 m/s.
  - With no target it flies level the way the camera faces (along the line of sight it went into the ground: the chase camera looks down).
  - proj_dragoon is collide 0: aimed at a firebat's legs it passed through them.
  - Tested: hits for 10 (proj_dragoon, source 2) + 10 (exp_dragoonProj splash, source 3), the real Dragoon's damage.
- Dragoon / Reaver shields:
  - cProtossShield: +0 flags (bit 1 = off, bit 2 = disabled), +4 actor, +8 amount, +0xC maximum, +0x10 cEffectPsiShield, +0x14 loop handle. The AI keeps it at cAIProtossDragoon +0x544, Create(actor, ACTOR_SHIELD +0x374, same), and calls On() in combat (Hook); Damaged takes damage off it (SetAmount; Off when empty).
  - Each Dragoon / Reaver vehicle (ACTOR_SHIELD 80 in ghostpc.not; SC1's value) gets its own, updated every frame.
  - tActor::Damage is detoured (sub esp, 8; push ebx; mov ebx, [esp + 0x20]): damage to them comes off the shield first (On(), Hit flash and deflectenergy sound, SetAmount), the rest goes to health. Off after 8 s without firing or hits; recharges 2/s after 5 s without hits.
  - Tested: shieldon + shieldloop on the first shot, 16 / 30 / 30 / 4 absorbed, then off.
  - debugdeath.cpp now skips a function already detoured (GHOSTPC_LOGDEATH copied our jmp and crashed).
- Dragoon collisions: cGoliathControl::Collide (vtable 22) crushes what it walks into (GibKill, 200 damage, containers); the AI Dragoon has none (cAIBaseWalker::Collide calls nothing), so it is skipped for the Dragoon.
- Dead Dragoon slid (the Goliath's walk sets friction 0): a dead one gets zero horizontal velocity and friction 4 every frame.
- Damage burn: the Goliath / Siege Tank templates' EFFECT4 DamDynFireMed (ctrldamage: fireMedium at marker 27). Protoss units use ProtBuildDam (ctrldamage: psiburn, cyan smokeSprite, marker 27; pylon, Scout). Dragoon and Reaver: EFFECT4 ProtBuildDam; the vehicle map preloads ProtBuildDam and psiburn.
- Reaver shield invisible (user), root cause:
  - A Protoss unit's visible shield is a second model: cEffectPsiShield::Init loads "<its model name>_shield" (renderModel_LoadInstance; +0xA4) and attaches it to the unit's model instance (cModelInst::SetAttachedModel; +0x200 bit 0 = has one). TurnOn shows it (clears its +0x14 bit 8).
  - The psiHemisphere (+0xA0) only draws the hit ripples, 12 slots.
  - The build has shells for the shielded units that shipped: Zealot, dragoon, hightemplar, observer, photoncannon(_out), purifier, pylon. Each is the unit's mesh on its skeleton, one material psiShield, about 0.1-0.4 m outside the body. There is no reaver_shield.
  - scripts/make_shield_models.py writes game/3D/Models/reaver_shield.nod from Reaver.nod: both materials psiShield, every vertex pushed 1.0 model units along its normal (0.5 was a little tight, user). Same skeleton, skinning and UVs. User: it shows.
  - Along the way: Create caps the starting amount at the maximum (+0xC), still 0 then, so shields started empty and charged for 40 s; now they start full.
- Damage burn points: the Dragoon and Reaver models have no type-27 markers, so ProtBuildDam's psiburn sat at their origin.
  - cEffectCtrlDamage makes one effect per marker (cModelInst::GetNumMarkers) and the smoke places itself with cModelInst::GetMarkerPos. Both are detoured (mov ecx, [ecx]; xor eax, eax; test ecx, ecx / push ebp; mov ebp, esp; and esp, -16).
  - For these two vehicles type 27 has two points on top of the body: Dragoon (0, 0, 2.1), (0, -2, 2.6); Reaver at its 0.75 scale (0, 1.5, 8.4), (0, -3, 7.1), from the tops of their vertex clouds.
  - Model markers found: Dragoon: fire (2) #0 (0, 0.14, 2.09), leg IK 0x13 / 0x15 #1-4 at the four feet. Reaver: fire (2) #0 (0, 5.76, 0.04) (on the ground: why its tank arc pointed underground), 0x13 #1-2.
- Dragoon aiming, second pass (user: inconsistent, a shot at the ground ahead went off to the right; wanted a cursor):
  - Every frame while driven, the aim point is where the line of sight (camera +0x8 to +0x74) meets the ground at its feet' height, at most 60 m out (or 60 m out level, looking up).
  - A live AI unit within 5 degrees (was 12) and 50 m is locked instead: target marker on it, aimed at its GetTargetPos, led.
  - Cursor: the aim path display (cGrenadeArc via AimPath, shared with the Reaver) from its fire marker to the aim point.
  - Tested: with the default camera the point is about 12 m ahead; shots there hit the elite firebat and hydralisk (direct and splash).
- Crash on quitting the vehicle level (user): GhostPC's DragoonShields read a freed thing.
  - Quitting frees a level's things before the world pointer changes, and the Reaver / console / Dragoon lists were rebuilt only every 30 frames.
  - Now each listed thing's GUID and model instance are kept when it is found. PruneThings (start of every VehiclesUpdate) drops any that gameThing_FindGUID no longer returns, and shields of gone things are dropped.
  - The marker detours compare model instances with the cached ones instead of reading the things.
  - Scripted presses do not reach the pause menu, so the quit itself is the user's test.
- Camera view: tSimCamera +0x8 / +0x74 are the target position / look point. The actual view (on screen) is the matrix at +0x210: rows right, forward +0x220, up +0x230, position +0x240. It eases after the target, trailing it by up to ~40 degrees while the mouse orbits it. The Dragoon now aims along +0x240 / +0x220 (CameraView). Tested: shots follow the view after turning the camera.
  - (renderCamera_pCurCamera is not valid outside rendering.)
- Goliath chaingun (FireChaingun): along gun marker (2, i)'s matrix (the torso, turned by IK to the camera) or at the lock-on target's GetTargetPos, spread +-5 degrees, instant-hit (gameProjectile_InstantFire). The Dragoon has no torso to turn.
- Dragoon placement: z -0.3, the field floor. An idle player walker is not simulated and stays where it is put; at 0.1 it floated (user). Needs the map recompile.
- Vehicle crosshair: vuiTarget_Draw draws a vehicle reticle only when the active control's +0x70 bit 2 is set (cControl::SetReticle(1); only cGunControl::Mount and Nova's gun modes call it), chosen by control type +0x10: 4 = cUITarget::DrawGoliath (DrawTurretReticle, marking the lock-on target), 5 = DrawTankReticle, 6 = DrawTurretReticle, otherwise cUITarget::Draw. Nothing set it for the Goliath, so it never had one. The Dragoon sets it every frame while driven (type 4).
- Balance (user; Ghost's damage scale is not SC1's):
  - Reaver: 600 health, shield 120.
  - Dragoon vehicle: 240 health, shield 120.
  - Dragoon shot (AI and vehicle): 40 direct + 20 splash. vehicles.cpp's tActor::Damage detour sets a proj_dragoon hit to 40 and its exp_dragoonProj splash to 20, once per explosion and unit (dragoonProjDam is 10 within 3 m, sometimes landing several times at once). Projectiles without PROJECT_DAMAGE hit for 10.
  - Tested: the vehicle hit a hydralisk for 40 + 20, splash 20 on a Marine / firebat / zergling; the Zoo's Dragoon hit Nova for 40 + 20.
- Player vehicles on the vehicle map (Global.not, code):
  - Vulture_player: 80 health, armor 0.6 (normal / fall / psi / fire / electric). Gun: proj_vultureGun (120 m/s, 10 direct) every 750 ms, explosion vultureShotExp -> vulShotExpDam 45-50 within 7 m (fire); the AI's proj_vultureGunAI 25-30. B: spider mines.
  - Goliath_player: 275 health, armor 0.1 / 0.3 / 0.3 / 0.3 / 0.5 / acid 0.5.
    - Chaingun: instant-hit (gameProjectile_InstantFire) 10 per bullet, 100 m, one every 50 ms (+0x1D4), +-5 degrees spread.
    - Rockets: proj_golMissilePlay (homing, 30 m/s) every 500 ms (+0x1D8), explosion expGolMslDamPlay 50-60 within 2 m (fire).
  - Arclite_player (Siege Tank): 900 health, armor 0.1 / 0.1 / 0.3 / 0.3 / 0.5 / 0.5. Drive shell 200 within 4 m, siege shell 1000 within 9 m (fire, explosion only).
- Goliath controls: B = dismount (+0x1B0 = 5), LT = rockets (+0x1B8 = 6; a pair from markers 3 / 4 per press, 500 ms apart, homing on the lock-on target), RT = chaingun (+0x1B4 = 7, held, 50 ms).
- Goliath chaingun 5 per bullet (user's balance) for the base AI Goliath (template "Goliath": cAITerranGoliath::AnimationCallback -> FireMachineGun(accuracy 0.7, damage 10) -> FireInstantProjectile, source 2) and the vehicle (Goliath_player: InstantFire 10, source 2), in the tActor::Damage detour (source 2, amount 10, attacker named Goliath / Goliath_player).
  - Unchanged at 10: Goliath_232 (cAITerranGoliathBoss::FireGun1 / 2, accuracy 0.85), the cAIBossGoliath bosses, and the other AI variants (Goliath_elite, _hauler, _rebel, _222c: other templates on the same AI).
  - Tested: the vehicle's bullets hit a zealot for 5, the Zoo Goliath hit Nova for 5.
- Dragoon crosshair, second part: cUITarget::DrawTurretReticle for a Goliath-type control (type 4) places it 8 m along the vehicle model's marker type 2 number 5 (the Goliath's sight), from cModelInst::GetMarkerMatrix without checking the result. The Dragoon has none, so it landed off screen. The GetMarkerMatrix detour supplies it for a Dragoon: at its fire marker, forward at the aim point. Logged as requested; not yet seen in game.
- Dragoon crosshair offset (user: the shot landed below it): DrawTurretReticle draws at sight marker + 8 m along its forward axis + 2.8 m along its up axis (+0x20; the Goliath's sight is above its guns).
  - The supplied marker now sits at aim point - 8 x forward - 2.8 x up, so the crosshair falls exactly on the aim point, where the shot goes.
  - DrawTankReticle (control type 5) instead draws lines along marker 2 #0 (the cannon) toward marker 2 #4.
- Reaver crosshair (user: the rings were under the ground, pointing down, like the tank's).
  - The aim-path display (cGrenadeArc) steps along its path until cGrenadeArc::Collide meets something, and on this level it never met the ground. The Reaver's and Dragoon's aim paths are removed.
  - The Reaver now gets the Dragoon's crosshair. vuiTarget_Draw's one call (vui_Update +0xBB; shell.cpp's SplitTargetDraw already redirects it, so it is chained) goes through VuiTargetDraw: while a Reaver is driven, its control shows type 4 (Goliath) with +0x70 bit 2 for that call, so DrawGoliath / DrawTurretReticle draws it.
  - The GetMarkerMatrix detour supplies the Reaver's sight marker 2 #5 so the crosshair falls on the unit its next Scarab goes for (GetTargetPos), or on the ground 20 m ahead.
  - Tested: no crash riding, firing and exiting; not yet seen on screen.
- Zoo: player turrets placed (user's annotated overview, 8 px/m from the door labels): Turret_player (Gauss) at map (20.1, 207.4) = world (-22.4, -85.6), facing north; FlameTurret_player at map (208.9, 146.1) = world (166.4, -146.9), facing north-east (yaw 315).
- Zoo prop Reaver at the vehicle's size: MAPtoNSD rejects a MODELSCALE placement override ("Unsupported or absent template property: nod_prop_143"). Instead GhostPC's reaver_prop_small (ghostpc.not: basedon reaver_prop, MODELSCALE 0.75, radius 2.85, height 3.75).
  - make_zoo_map.py now places GhostPC templates as make_vehicle_map.py does: archive ghostpc.not, nod_template_archives "ghostpc.not" on every placement. Footprint / effects come from the base template, scaled.
  - The level script's DEMOANIMS props are found by AI class, so the new name still works. Validated with MAPtoNSD (147 placements). Needs the Zoo compile.
- Grenades (unused ones):
  - Four inventory items (ePlayerItems 0-3), each a class whose constructor only sets a HUD name, an icon and a projectile template. All share tPlayerItemGrenade::Activate -> cPlayerControl::FireGrenade (the throw; aim arc cGrenadeArc).
    - canister (frag): HUD_CAN_FRAG, ItemCanisterIcon, templates canister / canister_bounce / canister_roll.
    - smoke: HUD_CAN_SMOKE, ItemSmokeGrenadeIcon, smokeGrenade.
    - sticky: HUD_CAN_STICK, ItemCanisterStickyIcon, stickyGrenade.
    - hellfire: HUD_CAN_HELL, ItemCanisterHellfireIcon, hellfireGrenade.
  - Data present: all templates, icons (global.xpr), HUD texts (game.nls), explosions and their effects (game.nfx).
    - smokeGrenade -> smke_Canister: 20 s smoke + volumefog fogGrenadeSmoke, no damage.
    - stickyGrenade: PROJECT_FLAGS 0x204001 (frag 0x20001F), grenadeLightTrail, the frag explosion exp_Canister.
    - hellfireGrenade -> exp_Hellfire (20 s): effect class cEffectExplosionHellfire, whose SpawnAll finds things in a sphere (tCollisionQuery::ThingsInSphere) and sends seekers at them; Update creates exp_Seeker at each (expSeekerDam: 70-80 fire within 0.5 m, force 200).
  - Sounds: expCanFrag.xsb and expCanSmoke.xsb are in the build. expCanSticky (exp_Hellfire's and exp_Seeker's foley) is not: hellfire would be silent.
  - Switching: cPlayerControl::ProcessUIControls -> tPlayer::SetNextPlayerItem, and the item select UI (cUISelect Next / PrevItem; cUIInventory::DrawGrenadeList).
  - Getting them:
    - INVENTORY tokens: canister smokegrenade stickygrenade spidermine hellfiregreande (sic) firefly mantis. Every mission in ghostsp.nsc gives only "canister 3".
    - Pickups (cPowerupCanister::GetItemType, by thing name): powGrenade -> frag, powCanisterSmoke -> smoke, powCanisterSticky -> sticky; it checks "powHellfire" but the template is powCanisterHellfire, so the hellfire pickup gives nothing (item 10).
    - powCanisterConcussion / Distract / EMP / Flash / Irradiate / Stasis have no item at all.
    - Placed in the build's levels: only powGrenade (1_2_1, 1_2_1a, 2_2_2a, Zoo), plus 2 powCanisterConcussion in 8_1_1a.
- Level script command "inventory <item> <count>" (levelscript.cpp): gives every local player that many of an item 10 frames after the level loads (tPlayer::SetPlayerItemAmount), as a mission's INVENTORY line does. Items: canister 0, smokegrenade 1, stickygrenade 2, hellfiregrenade (or the game's "hellfiregreande") 3, spidermine 4, firefly 5, mantis 6.
  - The Zoo's script gives 5 smoke, 5 sticky and 5 hellfire grenades (on top of the frag canisters).
  - Copied into game/Levels without recompiling the Zoo. Tested: given at load.
- NovaDT (original nova_DT.nod, Nova in Dark Templar armour, 32-bone old skeleton). Template Nova_DT (basedon Nova, EFFECT3 darkTemplar_eye_fx) has no motion set of its own: it inherits nova.nms, made for the 43-bone skeleton (wrong on 32 bones; nova_DT43.nod is GhostPC's conversion).
  - 24 Nova animations are old-skeleton ones (bones 0-31 only, using the old arm / leg range 14-24); all but two are unused by nova.nms:
    - a melee set "K": nova_K_attack1_1 / 1_2 / 1_3 (a three-hit combo), K_uppercut, K_Suppercut, K_Thrustattack, K_SThrustattack, K_SChargeup / SChargeDown, K_JumpDownAttack (+ _land), K_ThrowAttack (all 32 bones animated);
    - novaJumpAscent, nova_G_JumpDescent, nova_JumpRunTransition, novaCrouch_fire, nova_stealth_idle / standR / unhideR, nova_DISmountGunTurret_R, nova_VultureDismountLeft / Right;
    - used: nova_VultureMountLeft / Right.
  - These play correctly only on a 32-bone model: nova_DT.nod, novaFaceCovered.nod, artemis.nod. The K set (with DT's claws) looks like the Dark Templar Nova's melee.
  - artemis.nms (also 32 bones) uses the current animations.
- Flamethrower: complete in the code, just disabled.
  - tPlayer::CreateAllPlayerWeapons makes it as weapon 4 (tPlayer +0x470): HUD_WEAP_FLAME ("Perdition Flamethrower"), model name "gaussrifle" (Nova holds the Gauss rifle's model), foley weapflmthrwr (Sounds/weapFlmthrwr.xsb present), ammo type 3 (+0x44).
  - Its tPlayerWeapon +0x5F bit 1 is set, so weapon cycling (GetNext / GetPrevPlayerWeapon) skips it; ScriptPlayer::EnablePlayerWeapon clears it, and nothing calls that.
  - cPlayerControl::FireFlamethrower (from GaussRifleUpdate / GunCrouchUpdate when the weapon is 4): ammo, foley, breaks cloak, AI awareness, depletes ammo, rumble. FlameThrowEffect: effect controller PlayerFlameThrower (flameThrowLight / A / B / C at marker 2) + playFlmThwrLow, a raycast, tActor::Damage / tContainer::Damage on what it hits. FlamethrowerStop on release / weapon change.
  - Ammo: missions' AMMO line has maxflamethrower (ghostsp.nsc: "maxFlame 1000"). Pickup powFThrower (PU_Ammo_FThrower.nod, cPowerupFlameAmmo: fills ammo type 3).
  - "flamethrower" is listed under Inventory items in ghostsp.nsc's notes but is not an INVENTORY token; the same notes mark the smoke / stasis / sticky / concussion / EMP / irradiate grenades "CUT".
  - Level-script command "weapon <shockglove|pistol|gauss|flamethrower> [ammo]" enables a weapon and fills its ammo (debug GHOSTPC_WEAPON=<n> selects it).
  - Tested in the Zoo: weapon 4 enabled, selected, RT held at the Dragoon -> repeated 3-point fire damage (source 4) from Nova. Not seen: the flame effect, sound, HUD icon.

## 2026-11-05: Grenade and flamethrower pickups, hellfire seekers

- Hellfire grenade (exp_Hellfire, cEffectExplosionHellfire, 20 s): it spawns seekers at the things around it; each ends in an exp_Seeker explosion whose expSeekerDam does 70-80 fire damage within 0.5 m with force 200.
  - That force flung its targets across the map, spinning. In the Zoo, 11 seekers killed a zergling within two frames. They then kept following and exploding on the corpse, and each push sent the light body further.
  - vehicles.cpp SeekerDamage wraps cEffectDamage::UpdateDamage's simCollision_DamageThingsInSphere call (argument 5 = source thing, 11 = force); GHOSTPC_SEEKER_FORCE=<scale> scales an exp_Seeker's force.
  - The force is NOT the cause: with it at 15%, then 0, the zergling still flew off spinning (user). The force is back to the original (scale 1).
  - Also ruled out: cEffectExplosionHellfire::Update only creates exp_Seeker things at the target (gameThing_CreateFromTemplateAtPos); it moves nothing. exp_Seeker and exp_Canister are the same _explosion template apart from timer, foley and effect.
  - Found with SeekerHit / SeekerTrack's log: two seekers hit the zergling in one frame (force 0 passed). The first killed it, the second hit the dead body (+0x360 dead bit already set). From then on the body flew one ballistic arc: z 2.4 -> 21 m over about 40 frames, about 40 m sideways, then down. It was one launch, not repeated pushes. Fix: tActor::Damage from an exp_Seeker on a dead actor returns 0 without calling the game. (The hit-reaction idea is unneeded: the actor was already dead.)
  - Not enough: with seeker hits on the dead body refused, the next zergling still went (three hits in frame 1925, two of them refused). Its body then slid along the ground at about 25 m/s with no friction, reversed at a wall (x 121) and kept going. Still no ragdoll: tActor::Kill only calls EnableRagdoll when +0x360 has 0x200000, and the zergling's flags were 10025040. Ruled out: cEffectActorBurn moves nothing (only Damage and particles), and cAIZergling::ShotDuringJump's knock-away is only 5 m/s (SetVelocity). Diagnosis now: tThing::SetVelocity / ApplyImpulse / ApplyAngularImpulse are detoured to log calls on a tracked victim (and actors set over 12 m/s) with the caller, and the tracker logs GetVelocity.
  - Cause, found with the movement log: simCollision_DamageThingsInSphere has two passes.
    - It damages the things in the sphere first, then blasts each of them: simCollision_CalcBlastForces (rigid body +0x158, strength = argument 11, the force) -> tThing::ApplyImpulse + ApplyAngularImpulse.
    - A seeker's 70-80 fire kills a zergling in the damage pass, so the blast lands on the fresh corpse, which has no friction or drag. Four seekers in one frame: 4 x impulse |252| and angular |94|, velocity (29, -10, 33) m/s, about 20 m/s^2 of gravity: up to 28 m, onto the skybox.
    - Fix: while SeekerDamage runs (flag g_inSeekerDamage), the ApplyImpulse / ApplyAngularImpulse detours skip dead things. Living things are still pushed at the original force.
    - Confirmed (user, Zoo): the zergling died normally; the log showed all 8 blasts on its body skipped. (The run in between, which still flew, was not logged; probably an older executable.)
    - The earlier test with the force at 0 should have stopped it; that run probably used the previous build.
    - The diagnostics (seeker hits, the victim's track and velocity, the movers with callers) now log only with GHOSTPC_LOG_SEEKER.
  - Earlier suspect, kept for the record: the walker's hit reaction (KNOCKBACK / BLOWBACK / KNOCKUP) re-triggered by many fire hits a frame. vehicles.cpp SeekerHit / SeekerTrack log the first 4 seeker victims of a level for 3 s (each hit's type / amount / direction / region / force, then position, facing, dead flag, walker mode +0x374, AI state +0x18).
- Grenade pickups: cPowerupCanister::GetItemType maps a pickup by template name: powGrenade, powCanisterSmoke, powCanisterSticky, "powHellfire".
  - Global.not's hellfire canister is named powCanisterHellfire, so it gave nothing. GhostPC's ghostpc.not adds powHellfire (basedon powCanisterHellfire).
  - Zoo placements:
    - every boss room's supply row gains smoke, sticky and hellfire (the rows are now spaced 1.1 m so they fit);
    - the spawn room gains one of each along its south wall.
- Flamethrower pickup: powFThrower (cPowerupFlameAmmo::Activate, 0x5dc140) only fills ammo type 3.
  - vehicles.cpp detours it (first 9 bytes, `mov eax,[esp+4]; push esi; lea ecx,[esp+8]`). After the original runs, it clears bit 1 of +0x5F on the picking player's weapon 4 (ScriptPlayer::GetPlayerPointer), so the flamethrower becomes selectable.
  - In the Zoo it lies against the Lurker boss room's east wall, behind the boss (user's choice).
  - First test (user): the weapon was granted, but the pickup stayed, with no pickup message. cPowerupFlameAmmo::Activate only takes the pickup (ammo +50, AddPickupFX "POWFTHROWER", returns 1) while GetAmmo(3) < GetMaxAmmo(3) (tPlayer +0x444[type]); otherwise it sets the minimap hint POWINVFULL and returns 0. The Zoo's AMMO line has no maxflamethrower, so the maximum is 0 and the pickup always counted as full.
  - That diagnosis was wrong (logged on the next test): the Zoo's flame maximum is 200, and ammo starts full (200 of 200; a mission start fills every type). So the original said "Inventory Full" and left the pickup.
  - Also wrong: "the flamethrower is disabled". With GHOSTPC_LOG_WEAPONS (player 1's weapons' +0x5F flags and ammo at frames 5-300), every weapon in the Zoo has flags 00 from frame 5. tPlayer::GetNextPlayerWeapon skips a weapon whose +0x5F has bit 2 set, and ScriptPlayer::EnablePlayerWeapon clears it, so the flamethrower was selectable from the start. Weapons: 1 shock glove, 2 pistol (ammo type 0), 3 gauss (1), 4 flamethrower (3), 6 sniper (2).
  - Now:
    - Level-script "noweapon <weapon>" sets bit 2, 10 frames after load; the Zoo hides the flamethrower (tested: flags 02).
    - The weapon pickup is GhostPC's powFlamethrower (ghostpc.not, basedon powFThrower), against the Lurker room's east wall.
    - FlameAmmoActivate reads the pickup's name (cPowerup +0xC script handle -> ScriptThing::GetThingPointer, as cPowerupCanister::GetItemType does). Only powFlamethrower enables the weapon, and it is always taken then: through the original when there is room, else with the original's pickup message (ScriptWorld::AddPickupFX("POWFTHROWER", 0xD, 50, the player's position)) and 1, without calling it ("Inventory Full").
    - Plain powFThrower is flame ammo only; every boss room's supply row has one (k = -4, user).
    - A 0 maximum (a mission without one) still becomes 1000 first.
    - The Zoo needs a compile for the new placements.

- Smoke grenade (src/smoke.cpp): smokeGrenade -> smke_Canister has no damage and no AI effect, so GhostPC tracks each smokeGrenade projectile (only new GUIDs are checked each frame) and puts a 20 s cloud (7 m radius, 5 m half height) at its last position.
  - Burst: every living AI actor in it gets cAIInstance::Reset (target +0x2D4 cleared, out of the combat group, state NONE). Walkers whose vtable +0x258 is cAIBaseWalker::SilentStun are also knocked down: SilentStun starts walker mode 0x23 and AI state KNOCKDOWN (0x26), and its second argument goes to SetProneDuration (+0x428, ms: AnimationCallback ends it at stdTime +0x44 + duration); 2500 ms here.
  - While the cloud lasts, every 10 frames: an AI whose target is a player loses it again if either is inside a cloud. GHOSTPC_LOG_SMOKE logs each AI affected.
  - Research notes: AI state names (cAIInstance::GetStateName): 0x26 KNOCKDOWN, 0x29 STUNNED, 0x36 LOCKDOWN. Nothing sets STUNNED, and cAIInstance's stun / lockdown / combat-blind states are empty stubs (one ret at 0x4d81b0). tObject::Lockdown(duration) is the robots' freeze with the "lockdown" effect; walkers ignore it. cPlayerControl::SilentStun is Nova's takedown (lerp plus knockout motion), not a timed stun. No code calls the walker's SilentStun slot.
  - Walker controller modes (GetControllerModeName): 0x1E KNOCKBACK, 0x1F KNOCKFORWARD, 0x20 BLOWBACK, 0x21-0x23 unnamed ("FIXME!!"), 0x24 WINCE.
  - Not yet seen in game (the debug throw, GHOSTPC_ITEM "<frame> 1", threw nothing in the scripted run).
- Quit crash in the Zoo (2026-11-05, after the smoke / hellfire test): stdMemory_Free writing freed memory (eax dddddddd) from tRenderKeyTrack's destructor <- tMotionSet::Free <- tObject::Free <- tScene::Free <- tWorld::Free <- TerminateLevel. Motion sets are reference counted (Free decrements +0 and frees the key tracks at 0; the name at +4 is the gameMotion_apNames hashtable key), so some set's count went one too low. Not reproduced since (three later runs quit cleanly; none of them used the smoke grenade, the crashing run stunned 3 walkers). The smoke stun's walker mode 0x23 plays no motion (CMStartHitReaction case: prone timer +0x47C, ZeroVelocity, SetPinned 0), and GhostPC's PlayMotion calls pass no motion set (the Dragoon's GoliathPlayMotion passes it through), so no GhostPC code changes motion-set reference counts. Not yet identified. tObject::Free is now detoured to put "tObject::Free <name> (motion set <name>, refs <n>)" in the crash breadcrumb (the report's "GhostPC step"). Walker mode 0x23, which the smoke stun starts, is one of the plain hit-reaction modes (0x1E-0x24 all start through vtable +0xBD0).

## 2026-11-05: Psi Blade (user's plan: port the animations; a Psi Blade pickup dropped by Tanagazj turns Nova into NovaDT with the blade and its animations)

- Step 1 done: scripts/convert_nova_k_anims.py writes gameD\Anims
ova43_K_*.nad (12 files; the originals stay): the old-skeleton K melee set on the current 43-bone Nova.
  - Format: .nad v4 ends with event tags after the tracks: u32 count, count x (f32 frame, u32 eRenderKeyTag), u32 0. Plain animations end in 8 zero bytes (0 tags).
  - Bones: every track's bone renumbered with convert_nova_dt's OLD_TO_NEW. Every old bone keeps its parent under the map except the ponytail's last three (old 11-13, which all fall on new 12): their tracks are dropped, so old 7-10 drive new 9-12. The face bones get no tracks. All 12 files re-parse exactly and use only mapped bones.
  - The K set's tags fit the current melee system, cPlayerControl::MeleeCallback:
    - 0x1F / 0x20 / 0x21 -> MeleeHitCheck(tag), the hit (the shipped nova_G_melee has 0x1F at frame 6);
    - 0x18 / 0x19 set / clear cPlayerControl +0x68 bit 1;
    - 0x16 ends the action early (a StopAction check);
    - other tags (0x2, 0x3, 0x10, 0x11, 0x13, ...) are ignored there.
  - Per animation: attack1_1 (29 frames) hits at 7; attack1_2 (34) at 9 (0x20); attack1_3 (29) at 7 and 13; uppercut (39) has 0x18 at 2, 0x19 at 21; Thrustattack (19) has 0x18 at 7, 0x19 at 14.
- Leftovers of a cut Psi Blade:
  - Missions take "PSIBLADE 0 | 1" (cChronicle::Load: chronicle +0x10C bit 0), but cChronicle::SetPlayerData never reads it.
  - The developer console's quick list has "psiblade on", with no handler found.
  - Strings: LBENTRY_WEAPONPSIBLADE = "Psi Blade"; loading text 033_PsiBlade.nlt.
  - game.nfx has a player blade, controller psibladePlayer (psibladeGlow + psibladeBeam, partcylinders on marker type 3 index 0 = Nova's hand marker). cAIBossTanagazj::BladeOn = CreateEffect("psiblade", self) + AddEffect(effect, 1).
- Built (src/psiblade.cpp; scripts make_psiblade_nms.py, convert_nova_dt.py; ghostpc.not powPsiBlade; ghostpc.nfx psibladeNova):
  - Model swap at runtime: the player's cModelInst (tObject +0x2A0) keeps its bones, mixer and attachments; its cModel (+0) becomes nova_DT43.nod. Compatible because:
    - same skeleton and markers (cModel +9 bones, +4 shaders);
    - the instance block (cModel::NewInstance: 0x100 + 76 per bone + 4 per shader) was sized for nova's 9 shaders, and DT has 7.
  - cModelInst::Finalize(no matrix) then rebuilds the per-instance shader table (+0xA8, one per model material via the material's vtable +0x38). Without it, DT drew with Nova's materials (user's screenshot).
  - Motion set (tObject +0x2A4) -> nova_psiblade.nms: attack1 = K_attack1_1/2/3, gunmelee (base and StandRifle) = K_Thrustattack. gameMotion_LoadSet takes a reference before tMotionSet::Free drops the old set's.
  - Blade: GhostPC's psibladeNova (psibladePlayer's two parts at about 1/3 size) on marker type 58, which convert_nova_dt.py now adds to nova_DT43.nod:
    - bone 31 (the right forearm; elbow at model x -0.41, hand at -0.69, the gauntlet bulging about 9 cm under the arm at x -0.52 to -0.60);
    - local (-0.30, 0, 0.05): at the wristband's lower edge (user: -0.20 started mid-band, -0.27 a bit high, -0.32 cleared the band), outer side (+z in the palms-down rest pose);
    - beam axis (Z) along the arm toward the hand, tilted 10 degrees outward (user's mockup).
    - Before that, psibladePlayer on the hand marker (type 3) stuck out sideways and was about 1.2 m long (user: too big, wrong direction).
  - Weapon: SetPlayerWeapon(1) (the shock glove; its melee is attack1; no ammo).
  - Pickup: powPsiBlade (basedon powFThrower, PowerCrystal_01.nod at 0.6). The cPowerupFlameAmmo::Activate detour gives the blade for that name, shows "Psi Blade" (AddPickupFX "LBENTRY_WEAPONPSIBLADE") and returns 1.
  - Drop: the thing named Tanagazj is watched; when he dies (or is gone), powPsiBlade is created where he was last seen.
  - Taken away (user): on a level load or a restart (levelscript.cpp LevelLoadCount: gameWorld_LoadWorld + cSaveData::Restore, which reuses the player object), if the same player still has it: her cModel back plus Finalize, nova.nms back (balanced), tThing::RemoveEffect("psibladeNova").
  - The Zoo's effect list (make_zoo_map.py) also reads ghostpc.nfx and lists psibladeNova and its parts.
  - Draw order (user: a medkit behind Nova showed through the blade):
    - tWorld::Draw draws cEffectManager::Render, then tSortList::Render (the depth-sorted see-through things: pickups, alpha models). Particles write no depth, so anything in the sorted list painted over them.
    - Effect fields: the base cEffect::LoadData stores material (field 2) at +0x7C, flags (11) OR-ed into +0x50, priority (52) as a byte at +0x68 (field names: the table that MatchParam reads, 68 entries).
    - In tThing::DrawEffects, flag 0x80 adds the effect to the sorted list at distance 0, and 0x40 at the thing's camera distance.
    - psibladeNova and both parts have flag 0x40, so the blade is sorted with Nova. Not enough (user):
      - GHOSTPC_LOG_SORT=<frame> logs the list's entries for one frame (tSortList::AddSortThing / AddSortEffect detours).
      - Things with flag 0x200000 (the glowing pickups: powHealth, powEnergy) enter at key -1000 (tThing::Draw), and SortGroupCompare draws larger keys first, so they come last. The blade's key was Nova's distance (cModelInst +0x24, about 2.1).
      - Fix: the AddSortEffect detour (always installed) gives psiblade.cpp's blade effect (g_psiBladeFx) key -2000, so it is drawn after them. It is still depth-tested, so solid things in front hide it.
      - Confirmed in game (user).
    - Main-pass flags: 0x40000 = skipped; 0x80000 = drawn only out of Sight (PostSightRender draws it in Sight).
  - Shield (user: 40 points, only with the blade, on in combat, recharging 1 a second in combat and 3 out of it):
    - vehicles.cpp PlayerShieldOn makes a cProtossShield for the player (the Dragoon / Reaver shield code); PlayerShieldOff (cProtossShield::Destroy) removes it with the blade.
    - Its shell: nova_DT43_shield.nod (make_shield_models.py: NovaDT's mesh pushed out 0.09 m). That's the Zealot's median shell distance (its separate low-poly shell: median 9 cm, up to 24 cm off the body); the Purifier's is 12 cm.
    - Damage to anything with such a shield goes to the shield first (the tActor::Damage detour, no longer only Dragoon / Reaver).
    - Combat = being hit or hitting something (the damage's source has a shield); off after 8 s without either.
    - Recharge: Nova and the vehicle Reaver (user) 1 a second in combat, 3 out of it, with no wait. The Dragoon keeps 2 a second after 5 s unhit.
    - Tested by script: shield created with its shell (GHOSTPC_SHIELDS_ON forces every shield on after frame 200).
  - Debug: GHOSTPC_PSIBLADE=<frame> gives it; GHOSTPC_PSIBLADE_DROP=<frame> drops a pickup 2 m ahead.
  - Tested by script (no crash, all steps logged).
  - Not yet seen: the textures after the fix, the wrist blade, the K melee in use, the pickup's look, Tanagazj's drop, a restart.

- Restart vs. revive: cShell::State_RestorePlayer (shell state 69) revives the player where she fell (tActor::Revive). It restores the level-start snapshot (cSaveData::Restore) only when mainOptions_aLevelName is empty, i.e. a game started from the menus. A -level start (GhostPC's test runs) just revives in place (user: dying kept the blade, no return to spawn). psiblade.cpp now notes the bladed player's death and takes the blade away once she is alive again.
- Shield bar: cMiniMap::DrawPlayerBars draws Nova's health and psi bars (cPlayerInfo +0x24 / +0x28; 110 x 6 cUIBars at (42, 78) and (52, 68), HUD 640 x 480, y up). Its two calls (cMiniMap::DrawBorder, vuiMiniMap_Render) are wrapped: a third bar at (62, 58), blue, the shield's fill, while the drawn player has the blade's shield. (Only the minimap code calls cUIBar::Draw; the boss bars are vuiBar_New ones too.)
- Weapons: tPlayer +0x460[slot]: 1 shock glove (unarmed, attack1; HUD name HUD_WEAP_PISTOL, foley weapShockGlove: swing, whoosh), 2 pistol, 3 gauss (with sniper mode), 4 flamethrower, 5 grenade, 6 sniper. GetNextPlayerWeapon walks 1-4 and wraps from 2, so the shock glove is not normally cycled to. Weapon sound banks: wear (equip), remove, swing, outofammo.
- Sounds for the blade: ptssZealot has shieldon / shieldoff / shieldloop, swing, silentkill / 2 / 3, vox_silentkilled; ptssDarkTemplar (Tanagazj's foley via darkTemplar) has swing, shieldon / shieldoff. No blade-ignite cue (user: shieldon when her shield comes on, swing on attacks).
- Icons: vuiInventory_Startup loads a fixed table (RifleIcon_01-03, PistolIcon, MachineGunIcon_01-03, B_*Border_*, item and power icons); there is no Psi Blade or flamethrower icon. scripts/dump_xpr.py decodes an XPR1 pack to PNGs (header: "XPR1", total, header size, count, then (name offset, resource offset) pairs from offset 12; Xbox pixel containers: format bits 8-15, log2 sizes in 20-23 / 24-27, linear size word; swizzled in Morton order, DXT as is): exports\global_xpr (174 textures, index.html).
- K combo animations (keys: time, Euler angles in degrees, spline terms): attack1_1 = the kick (right thigh 184 / shin 142 degrees, body turning 80), attack1_2 = the wrist swipe, attack1_3 = the spin (body turning about 810). The blade's attack1 is now attack1_2 / attack1_3 (user: no kick). Gun melee stays Nova's own.
- Psi Blade weapon (src/psiblade.cpp; tested with scripted input and the game's own frames, GHOSTPC_SHOT):
  - Slot 1 (the shock glove, unarmed) is the Psi Blade once it is picked up.
    - The cycle: SetNextPlayerWeapon's wrap is patched from 2 to 1, SetPrevPlayerWeapon's bound likewise, and GetNext / GetPrevPlayerWeapon (only the weapon list uses them) are replaced by the same walks over 1-4.
    - Slot 1's +0x5F bit 2 is set every frame unless its player has the blade, so nothing changes without it.
    - Its name (+0) becomes LBENTRY_WEAPONPSIBLADE ("Psi Blade") and is put back when the blade is taken away.
    - The weapon list draws ItemCanisterEMPIcon for it: cUIInventory::DrawGun is redirected in DrawWeaponIcon and DrawWeaponUI (it draws slot 2 with the pistol icon, 3 and 6 with the rifle pieces, everything else with the machine gun's pieces).
    - GetWeaponAmmoText wrote nothing for slot 1, so the panel kept the last weapon's text ("56"). Both texts are now blanked for the blade.
  - Weapon fields: +0x454 = current weapon, +0x458 = cycle position (the player's choice), +0x45C = pending weapon.
    - Every melee swing switches to slot 1 for the swing: MeleeStart / Activate -> ChangeToBlade -> SetPlayerWeapon(1, 1, 0), which sets +0x454 = 1 at once and leaves +0x45C.
    - EarlyCompleteWeaponChange (from GaussRifleStop and others) then switches to +0x45C when it differs. That is how a swing returns to the gun.
    - Choosing the blade sets +0x458 = 1, and a SetPlayerWeapon wrapper sets +0x45C = 1 when slot 1 is set for a player who chose it. Without that, +0x45C was 0xCCCCCCCC (never set) and EarlyCompleteWeaponChange crashed in MountWeaponCallback.
    - "Blade selected" = has the blade and +0x458 == 1.
  - This build holsters the weapon while walking (+0x454 = 0, control state 2). Y (pad button 3) draws the chosen weapon into its aiming state (gauss: state 27). Fire (pad button 7, the right trigger, left click) only acts once a gun is drawn. B (pad 5) did nothing in the walk state in these tests.
  - So, with the blade chosen, a fire press in walk / sneak / crouch (2 / 4 / 5) calls cPlayerControl::MeleeAttack itself: TargetSelectMelee faces a target if any, PlayMotion attack1 (nova_psiblade.nms: K wrist swipe / spin), state 0x1E (30), back to walking about 1 s later. Tested: both swipes play with the blade.
  - Damage: hits from cPlayerControl::MeleeHitCheck (amount = control +0x229 per move) become 10 (Tanagazj's blade) when the blade is chosen; vehicles.cpp's tActor::Damage detour asks psiblade.cpp by return address. Tested: "melee hit by nova: 5 -> the blade's 10" (Nova's own melee does 5).
  - The blade effect is hidden (cEffect +0x50 bit 2) unless the blade is chosen.
  - Sounds (Sounds\custom\ghostpc, played with customsound.cpp's new CustomPlayOnce):
    - psiblade_swing.wav = protoss.xwb wave 0xD5, on the K animations' tag 0x13;
    - psiblade_shieldon.wav = 0xD6, when Nova's shield switches on (vehicles.cpp ShieldCombat);
    - psiblade_kill (~2, ~3) = 0xDE / 0xD9 / 0xDA, on a stealth kill.
    - The cue-to-wave pairing is read from ptssZealot.xsb's order (names: swing, shieldon, shieldoff, silentkill, silentkill2, silentkill3, shieldloop, vox_silentkilled, vox_damage_fire; waves D5, D6, D7, DE, D9, DA, DB, DC, DD). Not listened to.
  - Stealth kills: cPlayerControl::Activate's call of SilentStun is wrapped; with the blade but not chosen it returns 0.
    - SilentStun plays silentkill / 2 / 3 (motion modes 0x17-0x19), which nova.nms only has commented out, so Nova never did them.
    - nova_psiblade.nms gives them nova43_K_ThrustKill / SThrustKill: the thrusts with a 0x1F hit added at frame 10 / 8 (make_psiblade_nms.py add_hit_tag). The cut shock punch hit on 0x1F at frame 26.
    - Not tested (needs an unaware enemy from behind).
  - Taking it away also hides slot 1 and moves a player who had it chosen to the next gun (SetNextPlayerWeapon).
  - Debug: GHOSTPC_LOG_STATES logs the pad buttons pressed, control state changes and weapon changes.
- Psi Blade, corrections (user):
  - Melee is X (pad button 2; Q on the keyboard), not B. In GhostPC's default bindings B is E, A is Space.
  - The K combo: attack1_1 = the wrist swipe, attack1_2 = the kick, attack1_3 = the spin. My reading of the keys had 1_1 / 1_2 swapped; the blade uses 1_1 and 1_3.
  - Forked from melee, not replacing it:
    - nova_psiblade.nms keeps Nova's jabs as attack1 and adds the blade's swings as attack2 / attack3 (unused by nova.nms).
    - cPlayerControl::MeleeAttack's tObject::PlayMotion calls (two in it) are wrapped: with the blade chosen, mode attack1 (0xF) plays attack2 / attack3 alternately.
    - Every melee input (X, and the blade's fire press) swings the blade while it is chosen, and Nova's own melee otherwise.
  - The cycle no longer touches slot 1's +0x5F bit; setting it, to hide slot 1, broke Nova's own melee (the kick missing, user).
    - SetNext / SetPrevPlayerWeapon are replaced (not patched) by a walk over 1-4 in which slot 1 counts only for a player with the blade (and a weapon with ammo needs a maximum for its type, as the game checks).
    - Revert moves a player who had it chosen to the next weapon after the blade is gone.
  - No aiming with the blade: while it is chosen, the Y press (pad button 3; right click) is cleared each frame. Tested: Y no longer starts the aiming state.
  - Damage: 80 (Tanagazj's blade as GhostPC tuned it; the game's own 10 was the wrong reference, user).
  - Database entry:
    - Misc\ghostpc.nlx (ENTRY LBENTRY_WEAPONPSIBLADE 9100 24 ghostpc_PsiBlade.nlt defaultVid.bik) is inserted into game.nlx's LBTITLE_WEAPONRY block. xapi.cpp hands the game <save>\ghostpc_game.nlx when it opens game.nlx, as with game.nfx; game.nlx stays untouched.
    - The second column is the entry's id: cLogbook::IdFromName returns the entry's +0x64. With 0, as the always-unlocked weapon entries have, it found nothing usable.
    - Flags 24 = hidden + unlocked by a pickup, as the data logs.
    - The pickup calls UnInvisEntry + UnlockEntry(9100). Tested: "id 9100 unlocked".
    - The text Misc\ghostpc_PsiBlade.nlt is GhostPC's placeholder description (033_PsiBlade.nlt is a mission load text).
  - Confirmed by the user: dying removes the weapon.
  - A blade kill launched the zergling (user): cPlayerControl::MeleeHitCheck pushes its target after damaging it (ApplyImpulse / ApplyAngularImpulse), and the blade's 80 kills, so the push hit the fresh corpse, as the hellfire seekers did. The impulse detours skip a dead target when the push comes from MeleeHitCheck (by return address). Built; not yet seen in game.
- Psi Blade, round 3 (user):
  - Aiming: cPlayerControl::StartState(27) -- the gun's aiming state that right click starts -- becomes StartState(2) while the blade is chosen (a StartState detour). The weapon wheel only opens from the gun states (GaussRifleUpdate / GunCrouchUpdate -> OpenWeaponSelect, then UpdateWeaponSelect each frame, which closes it when the button is let go).
    - So with the blade chosen, walking / sneaking / crouching, holding Y (pad 3; right click) for 8 frames calls OpenWeaponSelect (control +0x68 bit 2, slow motion) from psiblade.cpp, and UpdateWeaponSelect every frame until it closes, also after it picked a gun.
    - Control button indices (cPlayerControl): +0x1D0 = 12, +0x1E4 = 3 (weapon), +0x1EC = 1 (melee).
    - The hold isn't tested by script (scripted presses last 2 frames).
  - CountAvailableWeapons (how many the weapon list draws; slots 2-4) is replaced by the same 1-4 walk.
  - Shield while cloaked (user: "an astronaut"): a player's shield stays off while her tCloakPower (tPlayer +0x4B4) IsWorking. ShieldCombat does not switch it on, and the per-frame update switches it off. It still takes hits and recharges.
  - Tanagazj's shield: none. cAIBossTanagazj::InitInstance creates his cProtossShield (AI +0x5AC) from the actor's ACTOR_SHIELD (+0x374), and neither Tanagazj, darkTemplar nor _Protoss sets one: 0 of 0 (logged). He has 600 health.
  - Quitting (user: crashed): the blade is taken away (Revert) before the level is freed, from a tSequence::TerminateLevel detour (first 9 bytes: test byte ptr [abs], 1; push esi; push edi). The quit crash itself left no log or dump.
  - Hang watchdog: no frame for 6 s (was 15) while focused now also writes ghostpc_hang.dmp (every thread's stack). The freeze when switching to the pistol near the Purifier left nothing: the log ended while the audio thread went on.
  - The Purifier (user: hit many times, it did not die):
    - tActor::Damage multiplies by armour (+0x3AC per damage type; the Purifier's type 0 = 0.40), and by 0.35 when the AI's vtable +0x54C returns > 0 (its blocking).
    - It then hands the amount to the AI's Damaged (vtable +0x190), then scriptMessage_Damaged, and applies what comes back.
    - cAIProtossPurifier::Damaged returned 0 for every melee hit while its shield (AI +0x554; amount +0x55C, max +0x560) had points, and the shield stayed at 90 (cProtossShield::SetAmount on it changed nothing either): invulnerable to melee.
    - Its Damaged slot is now wrapped: for melee (source 1) the game's still runs, then the hit comes off the shield (written directly, with Hit for the flash) and the rest goes to health. Gunfire keeps the game's result (its back / critical hit rules).
    - Tested: shield 90 -> 62 -> 34 -> 6 -> 0, then health 130 -> 108 -> 76 -> 44 -> 12 -> dead.
  - Debug:
    - GHOSTPC_LOCKDOWN="<frame> <name>" locks down every thing whose name starts with it (tObject::Lockdown, 10 min), e.g. the Zoo's Scout.
    - GHOSTPC_LOG_VEHICLE also logs each hit's result: dealt, health, the target's armour table and flags, and the Purifier's shield.
- Psi Blade, round 4 (user):
  - Shields: Nova's blade shield 80 (was 40). Tanagazj gets an 80-point shield: PsiBladeUpdate calls cProtossShield::Create(AI +0x5AC, him, 80, 80) when it finds him with none, and sets the amount (Create starts empty). Logged: "Tanagazj health 600 of 600, shield 80 of 80". His shell: Tanagazj_shield.nod (make_shield_models.py, 0.09 m).
  - Blade marker moved 0.5 cm in and 1 cm up the arm (user): local (-0.29, 0, 0.045).
  - Scripted input: GHOSTPC_INPUT entries take "<control>*<frames>" to hold a control (default 2 frames).
  - The weapon wheel from the walking state still does not open (user). Logging with a held scripted Y never showed pad button 3 held, so how this build reads Y is not settled.
  - GaussRifleUpdate's order: UpdateWeaponSelect when control +0x68 bit 2 (the wheel is open), ZTargetMovement until +0x16C, then UpdateActionButton, ProcessUIControls, CheckFloor / LedgeDropCheck, and the aiming.
  - Next (user's suggestion): a Psi Blade gun state -- the rifle state with weapon slot 1, no crosshair, no aim animation, melee on fire -- instead of keeping the blade out of state 27.
- Psi Blade gun state (user: right click held with the blade must open the weapon wheel, without the aim animation):
  - The StartState 27 -> 2 redirect is gone. Right click (control +0x1E0, pad button 2 here) starts GaussRifle (27) / GunCrouch with the blade like with any gun.
  - psiblade.cpp detours GaussRifleStart / GunCrouchStart and their updates. With the blade chosen, the game's own start runs (it sets up the aim camera; skipping it left the camera broken and the screen black for good, user), then the stand motion (mode 3) replaces the aim pose and SetReticle(0) hides the crosshair.
  - The blade's update: the wheel when open (UpdateWeaponSelect); right click let go -> walk (crouch); held over 250 ms (stdInput_GetJoyButtonHoldTime > 0xFA, as GaussRifleUpdate) -> OpenWeaponSelect. Fire swings the blade from it (not while the wheel is open).
  - A gun picked on the wheel gets the game's start again (aiming it); the blade picked while aiming a gun leaves the gun state.
  - Tested with scripted holds: the wheel opens with "Psi Blade" listed, the world renders, letting go returns to walking, picking the Gauss rifle aims it.
- Cloaked shields (user: Nova's shell showed over her cloak; cloaked Tanagazj was solid white; switching the shield off is not an option):
  - The shield stays on and keeps absorbing; only its shell is not drawn. vehicles.cpp ThingCloaked: the player's tCloakPower working, the model instance's override shader (+0x54, Tanagazj's Cloak sets "playercloak", UnCloak clears it), or tThing +0x48 bit 1.
  - The shell's own hide bit (cModelInst +0x14 bit 3, as TurnOff sets it) did not stop it drawing (user: still white). It is an attached model (cEffectPsiShield::Init: SetAttachedModel(shell, 0) on the unit's instance; slots at cModelInst +4 + 4 * slot). Now the shell is taken out of its slot while cloaked and put back when uncloaked, on death, and before PlayerShieldOff destroys it.
  - The shell's material gets a 64x64 "PsiShield_%x" texture made by the effect (filled 0x80808080, UpdateTexture draws the hits).
- Lockdown on the Siege Tank and the Vulture (user): cPlayerControl::TargetSelectLockdown takes only things with tThing +0x44 bit 3 (template thingflags 8: scout / observer). siegeTank_baseClass has 0x800000 only, TerranVulture none. levelscript.cpp LockdownFlags sets the bit on every thing whose AI is cAITerranSiegeTank or cAITerranVulture. The tank's own Lockdown / UnLockdown work; the Vulture's were cAIInstance's empty stub and now freeze its model (+0x14 bit 0x20000), zero its velocity and set state 54; UnLockdown unfreezes it and sets idle (2). Tested: both get the bit; GHOSTPC_LOCKDOWN on the Vulture calls the new Lockdown.
- Psi Blade mid-air attacks: the K set has nova_K_JumpDownAttack (22 frames, a plunge) and JumpDownAttack_land (20 frames, the landing strike), converted as nova43_K_*. Neither has event tags (no hit), and nova.nms has no mid-air attack mode, so using them needs a new jump-state attack. Also unused: uppercut / Suppercut (launchers), Thrustattack / SThrustattack, SChargeup / SChargeDown, ThrowAttack.
- Psi Blade air attack (research, nothing built yet):
  - Motion modes (gameMotion_apNames, set in gameMotion_Startup): attackdown 0x1F and attackdownland 0x20 match the K set's JumpDownAttack / JumpDownAttack_land. Also attackjumpslam 0x1E, superbladedive 0x2A, uppercut 0x1B, chargeattack 0x1A, superchargeattack 0x28, superuppercut 0x29. No motion set and no cPlayerControl code uses any of them (the "jumpattack" string is an AI vox cue).
  - Air states: JumpUpdate (rising; MoveRootPhysicsZ drives height from the jump animation's root), ForwardJumpUpdate, FallUpdate (AirControls steering, simPhysics_CheckGrabs ledges, CheckLanding). None reads the melee or fire buttons.
  - CheckLanding: ActorFindFloor under her, SetPhysPos onto it, vertical velocity zeroed, then cPlayerControl::Land. Land does the fall damage (tActor::Damage, falls only), a hit effect, ZeroVelocity, the landing foley and motion, and the next state.
  - JumpDownAttack (22 frames): root (bone 0) rises 0.93 m over frames 0-10, a leap before the strike. JumpDownAttack_land (20 frames): hips (bone 1) from crouched -0.46 back up. Neither has event tags.
- Psi Blade plunge (built, psiblade.cpp): fire while jumping / falling with the blade chosen.
  - Air states found at runtime from cPlayerControl::aStates (an entry holding JumpStart / JumpUpdate / FallStart / FallUpdate / ForwardJumpUpdate / StrafeFall*): mask 0xD80 (7 falling, 8, 10 jump, 11). From a jump she goes to falling (7), plays attackdown (0x1F) and is driven down at 15 m/s.
  - cPlayerControl::Land detoured (sub esp, 0x3C; push esi; mov esi, ecx): the game's landing first, then attackdownland (0x20) in the melee state 0x1E (as MeleeAttack: +0x22E = 0x201, +0x229 = 5, SetHoldTime), and 120 melee damage (tActor::Damage, source 1) to actors with a non-prop AI within 3.5 m (props, cAIDemoAnims, and players skipped: the first test hit the spawn room's rhynadon_prop and beacon).
  - PlayMotion returns 0 for JumpDownAttack_land (no event tags, so no hold time; the melee state ended at once): the hold is at least 650 ms (its 20 frames). Tested: plunge from state 10, strike held 0.73 s.
- Psi Blade hum: no Protoss sound bank has a hum or blade-loop cue (ptssZealot has shieldloop, shieldon, shieldoff, swing, silentkill(2,3), footsteps, aivox). By cue order shieldloop looked like wave 0xDC, but the user says it is not the hum (it may be the shield activation). psiblade.cpp loops Sounds\custom\ghostpc\psiblade_hum.wav while the blade is drawn (not cloaked, not dead); the user picked wave 0xC7 (5.8 s, flagged looping in the bank). exports\zealot_sounds holds every wave the Zealot bank references (0xD5-0xDE, 0x6B-0xA2) plus the looping Protoss waves (0xC5-0xC9, 0xE2, 0xE3, 0xE5).
- Blade effect hidden while cloaked (WeaponFrame: ThingCloaked on the player).
- Speed power: tPlayer powers are cloak (+0x4B4), sight (+0x4B8), speed (+0x4BC), offered when the power's +0x40 is set from the mission's POWERS line. The Zoo and vehicle test missions (ghostsp.nsc, its release copy, add_vehicle_mission.py, ghostmpcoop.nsc regenerated) now list cloak speed sight.
- Spider mines: the chronicle's INVENTORY token spidermine (cMission::InventoryTokensToItemCount) is the player's drivable spider mine (tPlayerItemSpiderMine, cSpiderMineControl). Both test missions start with 3.
- Flamethrower database entry: LBENTRY_WEAPONFLAMETHROWER 9101 (Misc\ghostpc.nlx, text ghostpc_Flamethrower.nlt, name "Perdition Flamethrower" in game.nls) unlocked by the powFlamethrower pickup. psiblade.cpp LogbookUnlock(entry) does both entries.
- Quit crash (user, again; no log after "taking the blade away", no dump, no Windows event): a silent end fits heap corruption (a fast fail skips the handlers). The earlier quit crash was a motion set's key tracks freed twice. tMotionSet::Free is now detoured: a set with 0 or fewer references (or freed memory) is not freed again and is logged with the object being freed ("motionset: ... skipped (step: tObject::Free <name> ...)").
- Co-op weapon pickups (user: both Nova and Echo get the flamethrower and the Psi Blade): vehicles.cpp FlameAmmoActivate uses a weapon pickup up only once every local player has the weapon (Activate returning 0 leaves the pickup). Until then it gives the weapon to whoever lacks it (the flamethrower with +50 fuel and its message) and stays; a player who has it passes over it. Single player is unchanged.
- The Psi Blade per player (psiblade.cpp): g_given is one record per local player (model and motion set to put back, blade effect, hum, plunge, gun state, death). Fire is read from each player's own pad: tPlayer +0x520 cPlayerInfo, +0x10 its pad index into aGamepads (0x1E4 each); stdInput_pCurrentGamepad is only the pad being updated. Give keeps the player's own motion set referenced and Revert puts it back (it used to load nova.nms; Echo's template is Nova_ice, based on Nova_red). Echo takes Nova's DT model until an Echo DT skin exists.
- dormant (level script, levelscript.cpp): "dormant <box> <ai class>": cAIInstance::Update (the AI's whole tick; test byte [aiMain_flags], 8; push esi) is detoured and skipped for that class until either player enters the box or one of them is damaged (AnyDamaged). The game's own hibernation (tThing::Hibernate) was no use: it hides the thing (+0x48 bit 1), frees its effects and drops its collision. The Zoo: the Hauler (cAIBossHauler, Hauler_Boss) and the Ultralisk (cAIBossUltralisk) in their rooms (make_zoo_map.py writes the lines).
- Psi Blade fixes after the user's test (2026-11-05):
  - Invisible blade, silent hum, blade "randomly" on: ThingCloaked counted a model shader override (instance +0x54) or tThing +0x48 bit 1 as cloaked, and both change on Nova for other reasons (her shell logged "cloaked" on and off). For players only tCloakPower::IsWorking counts now.
  - Hum volume: CustomLoopStart takes a volume factor; the hum plays at 0.25.
  - Plunge animation: nova_K_JumpDownAttack is a full move (root up 0.93 m over frames 0-10, then down 8.9 m by 22), so mid-air she "dived up". attackdown is now nova43_K_JumpDownAttack_air (frame 10 on, root translation zeroed). nova_K_JumpDownAttack_land starts with a somersault from that head-down pose (hips -116 to -340 degrees, lifted 1 m), so attackdownland is nova43_K_JumpDownAttack_land_strike (frame 14 on). Both via make_psiblade_nms.py trim_from, which also brings each rotation track into -180..180 (the keys' terms after the angles are tangents; the last key's are NaN in the originals).
  - Spinning on landing: cPlayerControl::MeleeUpdate turns her by control +0x1F0 (a turn rate) x the frame time every frame. A swing starts at 0; the strike, entered from the air states, kept a stale rate and spun her round (logged: facing changing every 3 frames, ending 155 degrees off). +0x1F0 is zeroed for the strike's 30 frames.
  - Impact effect (user: a shockwave on the ground): Misc\ghostpc.nfx psibladeShock (game.nfx's dragoonProjExp0, the Dragoon shot's flat blue ground ring: zoffset 0.01, width 0, type_flags 0x1042, widened to 5 m), made with cEffectManager::CreateHitEffectFromName(name, no thing, position, up, 0, 0, -99999) as CreateHitEffect makes its own. Confirmed by the user. CreateEffect by name never showed any effect, with or without an owner. CreateHitEffect hit type 10 (type 9: diveshock) is the cut blade dive's landing, bladeDive sparks plus superdiveshock, but superdiveshock is a 6 m tall electric wall raised 0.5 m: massive and floating (user). The controller psibladeImpact in ghostpc.nfx is unused.
  - Reach: 5 m and 4 m up or down (3.5 / 2.5 missed the Lurker); the log names the nearest enemy.
  - Shields: dying no longer matters (per player). Swing sound on the dive, the blade kill sound on impact.
- Mantis and Firefly: the chronicle's INVENTORY tokens mantis (cMantisControl, the drivable mine) and firefly; both test missions start with 3 of each besides the spider mines.
- Lurker claws (user: they rarely hit, and should do 40): cAIBossLurker::UpdateAttackMelee tests each armed claw (AI +0x594 / +0x590) with MeleeHitCheck(marker 3 #n to marker 5 #n, 15 damage): only a segment crossing her hit region counts. Its two 15.0 constants are patched to 40, and a wrapper hits a player whose body (1 m above her feet) is within 1.2 m of an armed claw's segment for 40, disarming the claw.
- Echo's Dark Templar armour (user's textures, 2026-11-06): scripts/make_echo_dt.py <darktemplar.png> <suit.png> writes EchoDarkTemplar.dds and Echo_DT_New.dds (DXT5; the alpha, the shaders' gloss / glow mask, taken from NovaDarkTemplar / Nova_DT_New: the painted PNGs have none, the helmet's was solid 255), nova_DT43_echo.nod (nova_DT43 with its seven materials renamed Echo_DT_* / echo_DT_glow / echoRifle_unarmed), nova_DT43_echo_shield.nod (nova_DT43's shell; cEffectPsiShield loads "<model>_shield"), and the seven materials in Materials\ghostpc.nsa, into game\ and assets\. The painted PNGs are in assets\source.
  - psiblade.cpp PsiBladeGive: a player whose model is nova_ice.nod (Echo, shell.cpp's player 2 skin) gets nova_DT43_echo.nod; others nova_DT43.nod. Debug: GHOSTPC_PSIBLADE2=<frame> gives player 2 the blade.
- tCloakPower::IsWorking is not "cloaked": it is true whenever the cloak is not disrupted (!IsDisrupted, +0x58 and +0x5C clear), i.e. nearly always (user: after cloaking the blade stayed invisible; shooting, which disrupts, brought the blade and shield back for a moment). The power's +0x44 is its on flag (tCloakPower::Start sets it, tPlayer::HasActivePower reads +0x44 of cloak / speed / sight). ThingCloaked uses +0x44 for players now. (The Dragoon-era Cloaked() had the same mistake.)
- Weapon switch from the blade (user: the gun would not fire until switched a second time): the weapon step (control +0x1E4, pad button 3, scripted "Black") calls tPlayer::SetNextPlayerWeapon(1), which only moves the chosen weapon (+0x458); the gun is drawn when aiming (GaussRifleStart -> GunSubModeStart -> SetPlayerWeapon(+0x458)), and GaussRifleStart ends with SetPlayerWeapon(+0x454). A gun is holstered while walking (+0x454 = 0), but the blade stays current (+0x454 = 1) while chosen, so that last call put slot 1 (the bare glove) back. psiblade.cpp HolsterBlade sets +0x454 to 0 before a gun state starts for a blade owner who chose a gun. Tested: the pistol is current in the aim state after one step.
- Shield shells and the white look (user: other units' shields flash white when they come on or are hit and are nearly transparent otherwise): GhostPC's shells do the same (cEffectPsiShield::UpdateTransition fades the shell's shaders from the switch-on pulse; a frame 3 s after the shield came on shows it nearly invisible). The lasting white was the cloak bug (shell detached and re-attached, shield switched back on by every shot) and, on Tanagazj, his cloak shader on the attached shell; a frame 2 s after switch-on can still catch the pulse.
- Turrets (user: the flame turret floats -- a map placement issue, user -- and its camera is poor; no health readout seated): cFlameGunControl derives from cGunControl and only replaces StandControlsUpdate, whose camera code is identical to the gauss turret's: the chase camera from avGunCameraPosOffset / avGunLookPosOffset indexed by the control's camera number (+0x1D0, cycled 0-1 by the camera button), offsets (0, -1.5, 3) / (0, -3.5, 3) behind and above the turret, looking at (0, 3, 2.9) / (0, 3, 2.7). Mount reads marker 15 (both models have it). Frames: the gauss turret (16 s after mounting) shows a first-person view behind its barrels; the flame turret (5 s after) a low third-person view beside its base. Not settled: the flame turret stands inside the Ultralisk's dormant box, so a warp there wakes it, and the turret was destroyed within seconds in two runs (god mode only covers Nova). Debug: GHOSTPC_TANAGAZJ_DEBUG=log with GHOSTPC_TANAGAZJ_DEBUG_POS warps; GHOSTPC_MOUNT=<frame> with GHOSTPC_MOUNT_NAME=FlameTurret mounts the nearest.
- Turret health on the HUD (user: no accurate readout seated): cMiniMap::DrawPlayerBars draws the rider's own health (cPlayerInfo::GetOldPlayer, Nova) when the active control is type 10 or 6 (cGunControl and cFlameGunControl are 6; cGoliathControl 4, cSpiderMineControl 8), else the active player's, a vehicle's own. A turret takes the hits itself (350 points) and its destruction kills the rider, so the bar stayed full until she died. psiblade.cpp HookPlayerBars patches the type 6 test (cmp al, 6 at +0x6B -> cmp al, 0xFF): turrets show their own health. Not yet seen damaged in game.
- GHOSTPC_TEST_GOD: vehicles.cpp's tActor::Damage detour now drops all damage to player-type things (type 7: the players and the vehicles they ride). levelscript.cpp's health restore missed one-hit deaths and riders (the flame turret was destroyed twice in tests, killing Nova). Tested: 16 s on the flame turret next to the awake Ultralisk.
- Flame turret camera, settled view (16 s after mounting, god mode): third person behind and left of the turret, about 45 degrees off its gun; the gauss turret's is straight behind its barrels. The flame turret is placed at yaw 315, the gauss turret at 0, and both use the same offsets: the camera offset may not follow the placement yaw. To test with the turret at yaw 0 (map change).
- Flame turret camera (fixed, user confirmed): cFlameGunControl's StandControlsUpdate (the only function it replaces; vtables otherwise identical) lacks cGunControl's block that, once the mount's hold time (+0x16C) has passed and +0x1CC is set, calls tSimCamera::BeginChase(avGunCameraPosOffset[camera +0x1D0], 0, avGunLookPosOffset[camera], 0) and clears +0x1CC. The camera stayed where Nova's had been when she mounted (logged eye 1.5 m up, 5 m off, never moving) until the camera button cycled it. vehicles.cpp wraps its vtable slot +0x10C to do that block first. Logged after: eye at the turret + (0, -1.5, 3) turned by its yaw. (Its yaw was not the cause: a map test at yaw 0 looked the same, reverted.)
- Flame turret sound (user: silent; Nova's flamethrower has sounds): it plays foley set playerTurret's "activate" (mode 1, with a handle at control +0x1E4: a loop, stopped on release) and "deactivate" (292); playerTurret.xsb has neither cue. Nova's flamethrower bank weapFlmthrwr (wear / swing / outofammo / remove) uses global.xwb waves 0xB3 and 0xB5 (0xB5 is the ignition, user); 0xB4 next to them is a looping flame (6.7 s, loop over the first 54468 of 89100 bytes). Sounds\custom\playerTurret\activate.wav = 0xB4's loop part (4.1 s). Exported to exports\flame_sounds.
- Debug mount log (GHOSTPC_LOG_VEHICLE): the camera's eye (tSimCamera +0x240), +0x220, the control's type and camera number, once a second.
- Flame turret sound volume (user: a bit lower): activate.wav rewritten at 60% (custom foley cues have no volume factor).
- Cerebrates in the build (search 2026-11-06: file names and contents across the game folder, Ghost.exe strings and symbols, all 1224 templates in Global.not): only text survives. Misc\199_MiniCerebrate.nlt (a logbook text: small Cerebrates control local groups of Zerg; destroyed, their Zerg frenzy and attack one another) and game.nls LBENTRY_ZERGMINICEREBRATE "Mini Cerebrate". game.nlx has no ENTRY for it (the Zerg list goes 190-198 and skips 199). No template, model, motion set, sound bank, AI class or "frenzy" code. cAIBossBrain (BOSSBRAIN) is the Vulcan template's boss AI, not a cerebrate.
- testEnemy_old (Global.not; entity_list: Zerg, 100 HP): basedon _Zerg, model szlachta.nod, motionset Szlachta.nms, foley zergZergling, walk 0.8 / run 3. Neither file is in the build (the Szlachta is a creature from Nihilistic's Vampire: The Masquerade - Redemption; the user's copy came from a piracy-site repack, so it was not used). It has no ACTOR_AI anywhere in its chain (_Zerg, _actor), so no AI drives it. The motion modes the actor code plays by itself: stand (3) and create (0x45) when it appears (tThing::Finalize / UnHibernate), hurt (0x74, tActor::PlayHurt), and on death deathquick (0x41) or deathslow (0x42) (tActor::KFXDeathAnimation, falling back to deathslow). With an AI (e.g. a GhostPC template basedon testEnemy_old with ACTOR_AI ZERGZERGLING) it would need that AI's set too: the zergling's modes are listed in zergling.nms.
- Spore and Sunken Colonies and irradiate (user: they do not die): by design. cAIZergSporeColony::Damaged and cAIZergSunkenColony::Damaged return the amount only for damage source 5 (Yamato) or 7 (Arclite) and 0 for everything else, so bullets, melee, explosions, fire and irradiate never hurt them; only those two calldowns destroy them (as SC1's Irradiate never hurt buildings). Damage sources (cEffect::GetDamageSourceFromToken, the nfx damage "type" token): none 0, mele 1, proj 2, exp 3, effect 4, yam 5, irr 6, arc 7, col 8. Irradiate: the calldown's irradDam (game.nfx: 2000 damage, type irr = source 6, radius 1.5 m) on the target, and cEffectActorIrradiate ("actorirradiate") spreading to actors within 2.5 m every 0.5 s (tActor::CanIrradiate: +0x360 bit 8, alive; both colonies qualify, ACTOR_FLAGS 0x04030068).
- ZangakarHickson.nod (template zangakarHickson_cut, basedon _ProtossCutscene, motion set dropship_cut): a cutscene rig with two characters in one model, Zangakar's mesh (his four DT_Zangakar materials, the same 3.78 x 0.95 x 2.71 m bounds as Zangakar.nod) and the rebel leader Hickson's (RebelLeader, RebelLeader_Head): 128 bones (Zangakar.nod has 74), 6318 vertices against 3515. Made for the dropship cutscene, where both are animated together.
- No irradiate on the colonies (user: irradiate cannot target buildings; the colonies have Arclite and Yamato variants): levelscript.cpp NoColonyIrradiate detours tActor::CanIrradiate (asked by cPlayerControl::ActivateCalldown and tThing::DrawCallout) and cEffectManager::IrradiateThing (the calldown's hit, tIrradiateCalldown::DoFire, and the effect's spread, cEffectActorIrradiate::Update) to refuse an actor whose AI is cAIZergSporeColony or cAIZergSunkenColony. Hooks install cleanly; not tested with an irradiate calldown (the Zoo's mission has none).
- The Szlachta in the Vampire: The Masquerade - Redemption demo (vampire_demo.exe, freely distributed; listed with 7-Zip, never run; its .nob archives are plain ZIPs): Resource.nob has 3d/Models/Szlachta.nod (35726 bytes), szlachta_noHead.nod, szlachtaHead.nod, 3d/Materials/SzlachtaCombo.tga / .nam / _dbl.nam, misc/Foley/enemySzlachta.nag, misc/AI/enemySzlachta.nai; Sounds.nob its sounds (idle, alert, attack, hurt, death, taunt, footsteps; a second voice set szlachta2_*). Its Global.not template names SZLACHTA.NOD, enemySzlachta.nag and SZLACHTA.NMS, but the demo has no Szlachta.nms and no Szlachta .nad animations (45 motion sets, 792 animations, none of them its). The model is .nod version 7 (Ghost's are 10; the old header carries the model's name): it would need converting.
  - Not embedded: Szlachta.nod v7 is 35 KB (2 materials, 34 bones, 657 vertices, 903 triangles; mesh parts BodyShape, HeadShape, L_legShape, InsidemouthShape...), no room for keyframes. Redemption keeps motion sets as separate .nms / .nad like Ghost: of the 38 motion sets the demo's Global.not names, 17 ship (those its levels use), 21 do not, among them szlachta, szlachtaboss, szlachtaeating, the gargoyles', warghoul, werewolf and player_id_0.
- The Szlachta in Ghost (user: better than nothing): scripts/convert_redemption_nod.py converts Redemption's .nod v7 to Ghost's v10 (renderModel_Load refuses versions below 10). v7 as worked out from Szlachta.nod: u32 7, material count, 32-byte names; u16 bones, u16 parts, u32 vertices, u32 triangles, u16 groups, u16, u16; 6 bound floats; 66-byte bones (local position, 3x3 rotation, inverse bind position, int16 next sibling / first child / parent: Ghost's 64-byte record with byte links); 32-byte part names; 40-byte vertices (position, normal, uv, f32 weight 1.0, u32 bone); u16 per-group vertex maps (one per vertex), u16 triangle lists (3 per triangle, local to each group's consecutive vertices), 8 spare; 28-byte group records (u16 triangles, vertices, ?; byte +9 part; byte +12 material; the last record cut short by the end of the file). UVs: V flipped (a UV plot over the texture fits only flipped; first in-game look had the texture misplaced, user). Units: Christof is 128 tall, scale 0.014 (the Szlachta about 1.35 m tall). Groups over 20 bones are split (Ghost palettes); one vertex set of type 1 (palette-local bone); no markers. Files (game\ and assets\): 3D\Models\szlachta.nod, 3D\Materials\SzlachtaCombo.dds (its TGA, alpha kept: shader masked), ghostpc.nsa SzlachtaCombo / SzlachtaCombo_dbl (twosided), 3D\Motions\Szlachta.nms naming stand / create / hurt / deathslow / deathquick to 3D\Anims\szlachta_bindpose.nad (one frame, no tracks: the bind pose, arms out). Seen in game (GHOSTPC_SPAWN), user: looks good. The Zoo map has testEnemy_old where ultralisk_cut stood (needs a compile).
- Debug GHOSTPC_SPAWN="<frame> <template> [metres] [right]": creates a template in front of player 1 (psiblade.cpp).
- Psi Blade on Echo in co-op (GHOSTPC_PSIBLADE=160, GHOSTPC_PSIBLADE2=170, -coop): both players get the blade, each her own model (Echo: nova_DT43_echo.nod, the blue armour), blade effect, 80-point shield and HUD shield bar; the blade motion set shared (one reference each).
- Level restart with both blades (co-op, GHOSTPC_RESTART=<frame>: tSequence::RestartSinglePlayer on mainSequence, the call the pause menu's Restart ends with; pending action +0x5C = 6, then DoRestartSinglePlayer: FreeWorlds, cSaveData::Restore): both players' blades are taken away (models, motion sets, shields reverted), no crash. With a -level start the restored objective is the first co-op mission's ("Investigate Cause of Refinery Explosion", 1_3_REFINERY): -level skips the mission select, so the snapshot holds the default mission. Entered from the menus (user; GHOSTPC_AUTOCOOP=8 for the Zoo) a restart keeps the Zoo's "Observe the enemy units".

## 2026-11-07: StarCraft sounds for the Reaver, Scarab, Vulture and Spider Mine
- Custom sounds can be keyed by template: Sounds\custom\<template>\<mode>.wav is checked before <foley set>\<mode>.wav
  (customsound.cpp), for a unit whose set others share (the Reaver uses the Dragoon's ptssDragoon; the Vulture's shot
  proj_vultureGun uses projTerrSgeTnk with proj_turretmine).
- Modes found with GHOSTPC_LOGFOLEY: mounting a vehicle = wear (cTankControl / cVultureControl::Mount); a vehicle's
  death = death (mode 83, tActor::KFXDeathSound); a shot or explosion = create (tThing::Finalize); a Spider Mine
  unburrowing for its target = vox_alert (0x61) and remove (0x14) with motion 0x49 (cAIMine::ASUpdateIdle).
- tThing::Finalize plays create through PlayFoleyMode and then the bank's createnoblock (0x140) straight through
  cGameMixer::Play (+0x11E); that call is redirected and skips the bank's cue when the thing has a custom create
  (the Vulture's old "pewpew", global.xwb wave 0xCC, played under the new shot).
- The Vulture's shot names EXPLOSION vultureShotExp / vultureShotExpAI: not templates but game.nfx effect controllers
  (vulShotExpDam, fire splash damage; vultureShotExp0, the fireball). With no template of that name the game makes the
  effect (no sound). ghostpc.not now has templates of those names (a template is looked up first) carrying the same
  effect as EFFECT1, whose create plays the impact sounds. (Bare explosions first: no fireball and no splash damage,
  user; the hit then did only the shot's own damage.)
- Files (StarCraft's, user's choice): reaver_vehicle_player\wear = PTrRdy00, \death = ptrDth00; exp_reaverScarab\create
  = PSaHit00; proj_vultureGun(AI)\create = TVuFir00; vultureShotExp(AI)\create, ~2, ~3 = tvuhit00, TVuHit01, TVuHit02;
  Tested (log): mount, Scarab hit, death, shot and impact (fire damage and fireball back). The Spider Mine keeps its
  own unburrow sound (user; TVuMin01 was tried as spiderMine\vox_alert and removed).
- Debug: GHOSTPC_KILL="<frame> <name>" kills live actors whose name starts with <name> (psiblade.cpp).
- Vulture engine (user's stock sounds; scripts/make_vulture_sounds.py makes them 22050 Hz mono at set loudness, loops
  crossfaded; sources in assets/source/vulture, the .m4a decoded with scripts/m4a2wav.ps1, Windows' MediaTranscoder):
  ghostpc/vulture_idle (chopride, -30 dBFS) loops the whole time a local player rides a Vulture (vehicles.cpp
  VultureIdleCheck). Moving: the game's engine loop playerVulture/lwalk_solid (jetengine; started when the bike moves,
  stopped when it comes to rest or on dismount) fades in over 350 ms with lwalk_solid_start (JETENGIN) once; stopped,
  it fades out over 600 ms and then lwalk_solid_stop (jet-powerdown, cut to 3.2 s) plays (customsound.cpp <mode>_start /
  <mode>_stop, FadeUpdate). Restarted while fading out (a bump stops the bike for a frame), the same voice is taken back
  up: no spool-up or power-down. Custom foley loops map the game's pitch (0.7 at rest .. ~2.4) to 0.85 .. ~1.35.
  Tested (log): set off, a bump, stop, dismount.- Shadows (vehicles.cpp; user: the Vulture's cut off, the Dragoon's gone at a distance): tThing::DrawFullShadow renders
  a thing through a light camera 2 x GetRadius (+0x50, when +0x4C is set) wide and 2.5 x +0x58 deep. The Vulture is a
  player-based thing, radius 0.5 m, its model 2.6 x 4.9 m: only the middle metre was drawn. Nova: 0.5 m, 1.7 m arm to
  arm. Now, for the call, the radius is the model's footprint (cModel +0xB0 tight bounds, farthest x / y, +15%) and
  the depth its height, when larger. FindIdealShadowLight and DrawShadow give no shadow while the model instance's
  detail (+0x24) is under 0.5; cModelInst::SetDistanceDetail sets it to +0x2C / (1 + 0.2 (distance - 5 m)), so
  shadows ended about 10 m from the camera; both now run with it raised fourfold (about 40 m), then put back. Both
  fixes for every thing (vehicles first, characters too, user). Debug: GHOSTPC_LOG_SHADOW (radius fields, bounds,
  light; per vehicle its light and detail once a second). Seen: parked vehicles shadowed at 30-40 m, the Vulture's
  whole shadow under it.
- Echo on the Vulture: novaVulture.nod has Nova riding it (NovaReduxFinal_gloss / _glo / _Lights / _hot);
  scripts/make_echo_vulture.py writes novaVulture_echo.nod with Echo's NovaIce_* (NovaIce_hot added to ghostpc.nsa),
  and vehicles.cpp EchoVultureModel puts it on the Vulture player 2 rides (put back when she is off). Seen with
  GHOSTPC_ECHO_VULTURE_P1 (debug: player 1 gets it).
- Vulture self-righting: a ridden Vulture tilted past 60 degrees (up vector +0xA0) is spun upright with
  tThing::SetRotVel (Havok angular velocity, rad/s) until within ~25 degrees. Not yet seen in a flip.
- Reaver turning while moving (user: its body stayed straight): it plays turnleft / turnright (reaver_turn_L / _R,
  which crawl like the walk with the head and body bent) whenever it turns, at the walk's 36 fps when moving, the walk
  only when going straight. Tested (log): walk -> turn right while moving -> walk.
- Custom sound range: one-shots are full volume within 5 m and silent at 60 m (gain squared); customsound.cpp
  SoundRange gives louder ones more: the Scarab's explosion (exp_reaverScarab) 160 m (user: faded too fast).
- Reaver destroyed (no death animation in reaver.nms): vehicles.cpp ReaverDeathCheck sets off the Pylon's death blast
  psiPylonExp (fire, shockwave, splash damage) and protossDeath smoke where it dies (CreateHitEffectFromName), and
  removes the hull 0.5 s later as cAIPsiDampener::Killed does (+0x364 |= 8, tThing::Remove(0)), once nobody rides it.
  Seen (log): blast and smoke at the Reaver on GHOSTPC_KILL; a ridden one is left to the game's death handling.
- Reaver movement sound (the Dragoon's special1, user: too loud): tObject::PlayFoleyMode ignores its float argument
  (only mode, thing, position and handle reach cGameMixer::Play); the Reaver plays it with a handle and sets that
  instance to 0.35 with cGameMixer::SetVolume, stopping the previous one first (the mixer keeps handled instances).
- AI and player vehicles: cAIInstance::IsValidTarget refuses a type-7 target whose actor flags (+0x360) have 0x400; the
  player vehicles' ACTOR_FLAGS 1536 (0x600: Goliath_player, Vulture_player, Arclite_player) include it, so AI never
  targets a player in a vehicle. GhostPC's Reaver and Dragoon templates now set ACTOR_FLAGS 512 (user: enemies
  ignored Nova in the Reaver); the original vehicles are unchanged. (A GHOSTPC_SPAWN'ed unit faces away and stays
  idle: no test of this.)
- Levels of detail (user: far off, the Hydralisk lost everything from the waist down): cModelInst::SetDetail picks
  level (1 - detail) x 4 (instance +0x28). hydralisk.nod's LOD 3 has no triangles on bones 1, 16-19 (group 10);
  scripts/make_lod_caps.py finds 48 skinned models whose reduced levels drop bones (zergling, mutalisk, overlord,
  Tanagazj from LOD 1, ...) and writes src/lodcaps.h; vehicles.cpp records their cModels in renderModel_Load and holds
  their instances at the cap in SetDetail (other models keep all four levels). Seen: the Hydralisk whole at 56 m.
  Debug: GHOSTPC_FULL_DETAIL (every model at level 0).
- Nova outlives her Reaver (user: she works it from the console): tActor::Kill calls the control's Killed and then, for
  a player thing with a player info (+0x520), cShell::RequestDeathMenu. The Spider Mine's Killed calls StartDismount(1):
  tPlayer::DismountVehicle(mine, 0, 1, 1), then Remove. cTankControl::Killed (vtable slot 16) is wrapped likewise for a
  driven Reaver (dismount, its +0x520 cleared); it then explodes and is removed (ReaverDeathCheck). Tested: killed
  while driven -> Nova back at the console, no death menu, blast, hull removed.
- Vehicle shield bar: cMiniMap::DrawVehicleHealth (from DrawPowersWindow) draws the vehicle's health at (45, 202), 70 x
  16. For a driven vehicle with a shield (Reaver, Dragoon) it is drawn 12 tall (its Resize immediate at +0x6E set for
  the call) and a 56 x 4 blue shield bar under it at (58, 202). Psi Blade Nova's bar stays at (62, 58). Seen in a frame.
- Reaver rules: no Scarab while it moves (turning is fine); cTankControl::Collide (crush damage) passes only contacts it
  moves into at 0.3 m/s or more. AI and vehicles: the 0x400 actor flag is cleared on every vehicle once a second.
  Untested in play. Debug GHOSTPC_LOG_LOS: every AI line-of-sight result on a Reaver target (user: AI hardly spot it).
- AI and driven vehicles (user: units saw the Reaver, put up "!", never attacked): sight (HasLOS) was fine. tPlayer
  +0x554 is the stealth meter (0 = plain sight; Nova 0.4 in normal light): cAIInstance::Search(int) raises the alarm
  by (1 - it), and SearchScore uses it too. A vehicle's is never updated (0, fine), but SearchScore came out 0 for it:
  FOVScore samples the target's sight points, which a vehicle model without sight markers fails. SearchScore (vtable
  +0x128) is wrapped in all 75 AI classes that use it: a driven vehicle scores 1 instead of 0 when within the AI's
  sight distance + 10 m and its line of sight test passes; its stealth is held at 0. (Holding it at 1, read as
  visibility, hid the Reaver completely.) Tested: warped among the test units, Marines and Zerglings took it as their
  target and hit it (about 19 s after it arrived, as their alarm rose). Debug: GHOSTPC_WARP_VEHICLE="<frame> x y z
  yaw" (moves player 1's vehicle; GHOSTPC_WARP is the player warp, another format), GHOSTPC_LOG_LOS (sight results,
  scores and each AI's target for a Reaver target).
- Reaver crushing redone: it moves by setting its physics matrix, so its physics velocity and a contact's speed are
  about 0 and the tank's crush never fit; a Reaver contact now kills a ground unit (actor, max health <= 600) on the
  side the stick drives it towards (collision damage). Tested: drove into the Zealot, Marines, Firebat: crushed.
- AI alarm on driven vehicles (user: "it's a huge reaver in front of them", detection was gradual): cAIInstance::ALAdd
  (vtable +0x368) is wrapped in every AI class; a driven vehicle raises the alarm 10x (VEHICLE_ALARM). Tested: the
  enemy tank fired 0.6 s after the Reaver warped in, Marines about 3 s later. All detection changes apply only to a
  player in a vehicle (control other than cPlayerControl); Nova on foot is as before.
- Zealot melee on vehicles (user: it ran in place by the Reaver, only its leap hit). Three distance tests measure to
  the target's centre, and nothing gets near the Reaver's centre (about 4.5 m wide, twice that long):
  - cAIProtossZealot::CheckCombatInRanges picks leap (Special2, 4.25-9 m), dash (Special1, 2.5-4.25) or swing from
    one centre distance; a detour after its fsqrt (+0x48) makes it the hull distance + 0.5 m against a driven vehicle.
  - ASUpdateCombatRanged (vtable +0x6F8) swings only within 4.25 m (fld [0x786F50], shared constant, image is
    relocated so rebase it); that load reads g_zealotReach, set per call (centre - hull distance + 2 m).
  - cAIBaseWalker::CanAttackFrom (vtable +0x9C0: AttackRange (+0x9B8) and a 10 m cap) measures to the hull too.
  - MeleeSphereHitCheck (the swing's sphere, used by the Zealot's BladeHitCheck, Zergling bites, Marine/Firebat bash)
    never found the Reaver (collision borrowed from the siege tank); its 7 call sites go through a wrapper that hands
    a miss to the target vehicle when the sphere touches its hull.
  HullDistance: the model's bounds (cModel +0xB0) as a box in the vehicle's frame. Tested: user saw the Zealot strike.
  Debug GHOSTPC_LOG_MELEE.
- Reaver crush tightened (user: a Zealot at its tail was crushed while turning): stick at least half way, and the unit
  must be at the leading half of its length.
- Release 2026-11-06 (scripts/make_release.py extended: the Vehicle Test, Vehicle Duel and Blade Arena levels with
  their sources in mapsrc\<folder>, their menu images, and GhostPC's files in game\ by an explicit glob list:
  templates, effects, database entries, Dark Templar Nova/Echo and blade animations, our shield shells only (the
  build's own Zealot / dragoon / hightemplar / observer / photoncannon / purifier / pylon shells are originals and
  stay out), Echo's Vulture, the Szlachta (user: include it), ruins textures, Sounds\custom. Left out: Misc\game.nls
  (the original string table; the build reads Strings\game.nls), assets\source, and the local Sabotage Protoss test.
  171 files, 15 MB; the previous zip kept as GhostPC_Patch_2026-10-29.zip.
- Flamethrower pickup model (user: as the spider mine pickup shows a spider mine, the flamethrower should be the
  rifle): scripts/make_flame_pickup.py builds 3D\Models\PU_FlameRifle.nod from gaussRifle_1.nod (the rifle as Nova
  holds it: 43 bones, set 0 rigid model-space vertices in groups 0-2, group 3 a skinned bit of her glove on set 1):
  set 0 and groups 0-2 only, one root bone, centred and resting on the ground, no markers. Each group's one-bone
  palette (+0x21) must be repointed to the root: left at 28 (Nova's hand bone, absent) it drew huge stretched
  triangles (user). powFlamethrower (ghostpc.not) uses it. Seen in the Protoss Arena's back room.
- Weapon pickups, second pass (user): the rifle spun (powFThrower's motion set PU_generic.nms: stand = PU_spin.nad;
  PROP_FLAGS as the spider mine's did not stop it), now motionset PU_still.nms (stand = PU_still.nad, one frame, no
  tracks; make_flame_pickup.py). Messages: cEffectManager::AddPickupFX(name, ...) sets the minimap hint to
  gameString_Find(name). The flamethrower pickup showed POWFTHROWER ("Flame Ammo"); add_pickup_strings.py adds
  POWFLAMETHROWER "Flamethrower" and POWPSIBLADE "Psi Blade" to Strings\game.nls, used as the two templates'
  DESCRIPTIONID and by FlameAmmoActivate (which now adds the weapon pickup's 50 ammo itself instead of calling the
  original, whose message is the ammo's). Tested in the Protoss Arena: "Flamethrower", then "Psi Blade".
- The rifle pickup lay at an angle, one end off the floor (user): as held, gaussRifle_1's rifle is pitched 14.5 degrees
  and rolled 3.5. make_flame_pickup.py now turns positions and normals by the pitch / roll (searched in half degrees)
  that make it lowest (0.09 m instead of 0.24) and rests its lowest point on the floor. User: works.
- Echo's HUD lost (user: if Echo is dead when the level restarts, her HUD stays gone): the hudtrace showed her dead
  flag (+0x360 0x10000000) still set after a pause-menu Restart (tSequence::RestartSinglePlayer revives only player
  1); a dead player's HUD passes are skipped, so it never came back. levelscript.cpp now revives any local player
  still dead at a restart as her button press does (cPlayerControl::UpdateDead: tActor::Revive(1, 1, 0)); the level
  script then places her. Tested (GHOSTPC_AUTOCOMP=1, GHOSTPC_KILL="900 Nova_ice", GHOSTPC_RESTART=1050): revived,
  her HUD back. While dead the HUD is hidden, as the game does for any dead player.
