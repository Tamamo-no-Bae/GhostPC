# GhostPC

A native Windows host for the **StarCraft: Ghost** Xbox prototype (May 2004 build). GhostPC.exe loads the
prototype's own `Ghost.exe` (the Xbox executable, with its `Ghost.map` symbols) into a Windows process and replaces
the Xbox libraries it was linked with: Direct3D 8 (NV2A) runs on Direct3D 9 with the vertex and pixel shaders
translated to HLSL, DirectSound/XACT on XAudio2, the Xbox controller on keyboard + mouse or an XInput pad, and the
kernel and XAPI calls on Win32.

It also fixes prototype bugs, restores cut content (boss AIs no level used, the Vulture's AI, profile deletion) and
adds the **Enemy Zoo**, a test level with every unit and boss of the build.

**You need your own copy of the prototype.** No game files are in this repository.

## Playing

Download the latest `GhostPC_Patch.zip` from [Releases](../../releases), copy its contents into the prototype's
folder (the one with `Ghost.exe`), and run `GhostPC.exe`. Controls, settings and the full changelog are in
[`GhostPC_README.txt`](release/GhostPC_Patch/GhostPC_README.txt).

## Building

Requirements: Visual Studio 2019 Build Tools (MSVC, x86), Python 3, and the prototype in `game\` (the build checks
the hooks against `game\Ghost.map` and copies `GhostPC.exe` there).

```
build.bat
```

builds `build\GhostPC.exe` and copies it into `game\`.

## Layout

| Path | What |
|---|---|
| `src/` | GhostPC: loader, D3D/audio/input/XAPI replacements, game fixes (`levelscript.cpp`, `shell.cpp`) |
| `scripts/` | analysis and test tools: symbolic disassembler (`gdis.py`), template reader (`notdump.py`), test runner (`runtest.ps1`), release packaging (`make_release.py`) |
| `levels/zoo/` | the Enemy Zoo: generator (`make_zoo_map.py`), install script, overview (`ZOO_OVERVIEW.md`, `zoo_overview.png`) |
| `release/GhostPC_Patch/` | the patch's readme (the zips themselves are on the Releases page) |
| `FINDINGS.md` | research notes: how the prototype works, every fix and why |

The Enemy Zoo is built with the community OpenNOD SDK:
[github.com/aknavj/nodsdk-scghost](https://github.com/aknavj/nodsdk-scghost).

## Debugging

GhostPC writes `ghostpc.log` next to itself (the previous run's as `ghostpc.prev.log`); a crash adds a report to
the log and a minidump, `ghostpc_crash.dmp`. Debug switches are `GHOSTPC_*` environment variables, listed in
`FINDINGS.md`.

## Credits

- jason098: reported the movie decoder crash fixed in the first release.
- aknavj: the OpenNOD SDK used to build the Enemy Zoo.

StarCraft: Ghost is a trademark of Blizzard Entertainment. This is an unofficial fan project, not affiliated with
or endorsed by Blizzard. The code is under the [MIT license](LICENSE).
