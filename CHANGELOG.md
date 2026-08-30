# Changelog

Fork of [asuparabekon/xray-monolith-bodycam](https://github.com/asuparabekon/xray-monolith-bodycam)
(itself a fork of [themrdemonized/xray-monolith](https://github.com/themrdemonized/xray-monolith)).
This file tracks what's added on top of upstream `bodycam-mt`, not upstream's own history.

Entries are added per meaningful change, whether or not it's build-verified yet — see each
entry's status tag.

## fade-system (branch, off bodycam-mt)

### 2026-08-30 — Configurable engine-side audio fade in/out — `verified`

Adds `sound_object:set_fade_out(duration_s[, curve])` and `set_fade_in(duration_s[, curve])`
to the Lua sound API (`curve`: `0` = linear, `1` = equal-power). Extends the engine's
existing fixed ~100ms fade-out (`stop_deffered()`) rather than replacing it — anything that
never calls the new setters is unaffected.

Built to eliminate audio-splice clicks in a custom Lua fire-loop system
(`snd_shoot_start` → `snd_shoot_loop` → `snd_shoot_end`), but it's a general-purpose engine
capability usable by any script.

Files: `src/xrSound/Sound.h`, `SoundRender_Emitter.h`, `SoundRender_Emitter.cpp`,
`SoundRender_Emitter_StartStop.cpp`, `SoundRender_Emitter_FSM.cpp`,
`src/xrGame/script_sound.h`, `script_sound.cpp`, `script_sound_inline.h`,
`script_sound_script.cpp`. Full technical writeup: `ENGINE_CHANGES_FADE_SYSTEM.md`.

Compiled clean in `DX11-AVX|x64`, no warnings tied to the changed files.
