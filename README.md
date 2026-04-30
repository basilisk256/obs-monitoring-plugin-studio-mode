# OBS Audio Cue

C++ OBS plugin adding inline **Monitor** buttons under each Studio Mode viewer (Preview / Program). Provides broadcast-console-style **PFL** (pre-fade listen) — click Monitor on Preview to route only that scene's audio to your headphones, with the live stream completely unaffected. Click Monitor on Program to also send program audio to your headphones (without changing what's on stream).

## Status: working

- **Per-pane Monitor buttons** appear inline under Preview and Program viewers in Studio Mode
- **Mutually exclusive** — turning one on releases the other
- **Cue follows the scene through cuts** — if you're monitoring Preview and you cut, the cue auto-switches to Program on the same scene so you keep hearing the same thing
- **Source restart on cut** — sources that have a "restart on activate" type behavior (browser source `restart_when_active`, media source `restart_on_activate`, etc.) get their activate handler fired cleanly on the cut, identical to clicking the scene normally outside Studio Mode
- **Crash-safe** — all state changes are checkpointed; if OBS crashes mid-cue, the orphan checkpoint is restored on next plugin load so your scene collection isn't left in a modified state
- **PANIC restore hotkey** — force-release everything immediately if anything ever feels stuck

## Required OBS setting

OBS Settings → Audio → Advanced → **Monitoring Device** must be set to your headphones / return device, not "Default". With Default, Monitor Only routes to nowhere and you hear nothing.

## Hotkeys (bind in Settings → Hotkeys)

- **Toggle Audio Cue: Preview pane**
- **Toggle Audio Cue: Program pane**
- **Audio Cue: PANIC restore (force release all)**

## How it works (under the hood)

When a Monitor button is on for a target pane:

1. The engine enumerates **every audio source** in OBS (excluding scene/group containers — those are filtered out because touching them propagates to all children and corrupts program rendering).
2. For sources in the cue target's scene:
   - Sources also in program → set to `MONITOR_AND_OUTPUT` (audible to you, still on stream)
   - Preview-only sources → set to `MONITOR_ONLY` + `obs_source_inc_active` so they actually emit audio (preview-pane sources are inactive by default)
3. For all other audio sources → set monitoring to `NONE` (silenced from your headphones; mute and active state are not touched so stream/recording is unaffected)
4. A 200ms re-assert thread defends against other plugins (e.g. `scene_mute.lua`) re-muting captured sources mid-cue
5. On cue release, every captured source's prior `(muted, monitoring_type, active_refs)` is restored exactly

### The cut-restart trick

Because we hold `inc_active` on preview-only sources for monitoring, OBS's natural `inc_active` at cut-time goes 1→2 instead of 0→1, so the source's `info.activate` handler (browser refresh / media restart / etc.) is never invoked.

The fix is universal across source types: when a cut promotes a source from preview to program, the engine `dec_active`s until refs hit 0, then schedules the matching `inc_active` calls on a detached worker that sleeps **120ms** (≈7 frames at 60fps). The intervening video tick observes refs=0 and fires `info.deactivate`; a later tick observes refs=1 and fires `info.activate`. Side effect: brief blank/freeze on the source during the inactive window — that's the cost of getting the proper restart.

### Crash safety

`mute` and `monitoring_type` get persisted to the scene collection JSON whenever OBS exits or auto-saves. Without safety, a crash mid-cue would leave temporarily-modified state stuck on disk.

Mitigations:

- Every state change writes `%APPDATA%\obs-studio\plugin_config\obs-audio-cue\active-checkpoint.json`
- On `OBS_FRONTEND_EVENT_FINISHED_LOADING` (after sources are available), if a checkpoint exists the engine restores each source from it, then deletes the file
- `SCENE_COLLECTION_CHANGING` / `SCENE_COLLECTION_CLEANUP` events trigger a panic restore so OBS persists the user's untouched state, not the modified one
- Clean cue-off / OBS exit also deletes the checkpoint

## Build

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release --parallel
```

OBS deps and Qt6 are downloaded automatically via `buildspec.json` on first configure.

## Install

Run from an **elevated** PowerShell:

```powershell
& 'C:\Users\griff\dev\obs-audio-cue\install-admin.ps1'
```

The script auto-kills `obs64.exe`, copies `build\Release\obs-audio-cue.dll` to `C:\Program Files\obs-studio\obs-plugins\64bit\`. This OBS install only scans Program Files — per-user paths are not used.

## Files

- `audio-cue-module.cpp` — `obs_module_load`, hotkey registration, frontend event hooks (checkpoint loader, scene-collection guards)
- `audio-cue-engine.cpp/.h` — capture/release/sync logic, 200ms re-assert thread, JSON crash checkpoint, scene-state debounce handler, force-reactivate machinery
- `audio-cue-dock.cpp/.hpp` — Qt button injection into `previewContainer` (by objectName) and `programWidget` (found via walking `previewLayout` for child with `label-preview-title` label class)

## Known cosmetic warning

`Failed to load 'en-US' text for module: 'obs-audio-cue.dll'` — the install script doesn't copy `data/locale/`. Harmless because the plugin doesn't use `obs_module_text()` anywhere.
