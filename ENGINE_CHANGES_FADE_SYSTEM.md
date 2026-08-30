# Engine changes: configurable audio fade-in/fade-out system

This document describes the engine-side (C++) modifications applied on top of
the stock `xray-monolith-bodycam` source tree, so they can be re-applied to a
future update of the bodycam build without having to rediscover them from
scratch.

**Purpose:** expose a configurable fade-out duration/curve and a genuinely new
fade-in capability on 2D sound emitters, used by the `MOD_TRUE_CYCLIC` Lua
script (`zzzzz_sound_loop_custom.script`) to eliminate audio clicks when
splicing `snd_shoot_start` → `snd_shoot_loop` → `snd_shoot_end` segments for
the Tarkov-style cyclic gunshot system.

**Status:** applied on top of `zoobi398/xray-monolith-bodycam` (fork of
`asuparabekon/xray-monolith-bodycam`), branch `fade-system` off `bodycam-mt`,
compiled successfully in `DX11-AVX|x64` via `src/engine-vs2022.sln`.
Originally applied by hand to a zip export of `bodycam-2026.8.4` on 2026-08-23;
re-applied as clean git commits on this branch on 2026-08-30.

## What already existed before this patch (do not re-add)

The engine already had a **fixed, non-configurable ~100ms linear fade-out**
mechanism, already exposed to Lua:

- `ref_sound::stop_deffered()` (`Sound.h`) → `CSound_emitter::stop(TRUE)`
- `CSoundRender_Emitter::stop(BOOL bDeffered)` (`SoundRender_Emitter_StartStop.cpp`):
  sets `bStopping = TRUE` instead of stopping immediately.
- `CSoundRender_Emitter::update_culling()` (`SoundRender_Emitter_FSM.cpp`), 2D
  branch: `fade_volume += dt * 10.f * (bStopping ? -1.f : 1.f);` — this IS the
  100ms fade (rate 10/sec → full ramp in 0.1s).
- Lua binding: `CScriptSound::StopDeffered()` (`script_sound.h` /
  `script_sound_inline.h`), registered as `sound_object:stop_deffered()` in
  `script_sound_script.cpp`.

This patch does **not** replace that mechanism — it extends it. Anything that
never calls the new `set_fade_out()`/`set_fade_in()` setters below keeps the
exact original behavior (0.1s linear fade-out, no fade-in, `fade_volume`
forced to `1.f` on every fresh play).

## Files modified (7 files)

### 1. `src/xrSound/Sound.h`

- `CSound_emitter` (abstract interface): added
  ```cpp
  virtual void set_fade_out(float duration_s, int curve) = 0;
  virtual void set_fade_in(float duration_s, int curve) = 0;
  ```
  right after `virtual void stop(BOOL bDeffered) = 0;`.

- `ref_sound` struct: added declarations
  ```cpp
  IC void set_fade_out(float duration_s, int curve = 0);
  IC void set_fade_in(float duration_s, int curve = 0);
  ```
  right after `IC void stop_deffered();`.

- Added inline implementations right after the pre-existing
  `ref_sound::stop_deffered()`:
  ```cpp
  IC void ref_sound::set_fade_out(float duration_s, int curve)
  {
      VERIFY(!::Sound->i_locked());
      if (_feedback()) _feedback()->set_fade_out(duration_s, curve);
  }

  IC void ref_sound::set_fade_in(float duration_s, int curve)
  {
      VERIFY(!::Sound->i_locked());
      if (_feedback()) _feedback()->set_fade_in(duration_s, curve);
  }
  ```

### 2. `src/xrSound/SoundRender_Emitter.h`

- Added 6 new fields next to `float fade_volume;`:
  ```cpp
  float fade_out_duration_s;
  float fade_in_duration_s;
  int   fade_out_curve; // 0 = linear, 1 = equal-power
  int   fade_in_curve;  // 0 = linear, 1 = equal-power
  float fade_out_elapsed;
  float fade_in_elapsed;
  ```

- Added virtual declarations next to `virtual void stop(BOOL bDeffered);`:
  ```cpp
  virtual void set_fade_out(float duration_s, int curve);
  virtual void set_fade_in(float duration_s, int curve);
  ```

### 3. `src/xrSound/SoundRender_Emitter.cpp`

- Constructor: added initialization of the 6 new fields right after
  `fade_volume = 1.f;`:
  ```cpp
  fade_out_duration_s = 0.1f;
  fade_in_duration_s = 0.f;
  fade_out_curve = 0;
  fade_in_curve = 0;
  fade_out_elapsed = 0.f;
  fade_in_elapsed = 0.f;
  ```
  These defaults exactly reproduce the old hardcoded behavior (0.1s linear
  fade-out, fade-in disabled) for any emitter that never calls the new
  setters.

### 4. `src/xrSound/SoundRender_Emitter_StartStop.cpp`

- `start()`: added a reset of the 6 fade fields to their defaults (needed
  because emitters are pooled/reused — without this, a reused emitter could
  carry over fade settings from a previous, unrelated sound):
  ```cpp
  bStopping = FALSE;
  bRewind = FALSE;

  fade_out_duration_s = 0.1f;
  fade_in_duration_s = 0.f;
  fade_out_curve = 0;
  fade_in_curve = 0;
  fade_out_elapsed = 0.f;
  fade_in_elapsed = 0.f;
  ```

- `stop(BOOL bDeffered)`: added a reset of `fade_out_elapsed` when entering
  the deferred-stop state, so the fade-out ramp always starts from t=0:
  ```cpp
  void CSoundRender_Emitter::stop(BOOL bDeffered)
  {
      if (bDeffered)
      {
          bStopping = TRUE;
          fade_out_elapsed = 0.f;
      }
      else i_stop();
  }
  ```

- Added the two new method bodies:
  ```cpp
  void CSoundRender_Emitter::set_fade_out(float duration_s, int curve)
  {
      fade_out_duration_s = (duration_s > 0.f) ? duration_s : 0.1f;
      fade_out_curve = curve;
  }

  void CSoundRender_Emitter::set_fade_in(float duration_s, int curve)
  {
      fade_in_duration_s = (duration_s > 0.f) ? duration_s : 0.f;
      fade_in_curve = curve;
      if (fade_in_duration_s > 0.f)
      {
          fade_in_elapsed = 0.f;
          fade_volume = 0.f;
      }
  }
  ```

### 5. `src/xrSound/SoundRender_Emitter_FSM.cpp`

This is the file containing the original hardcoded fade. Inside
`CSoundRender_Emitter::update_culling(float dt)`, the original 2D branch was:

```cpp
if (b2D)
{
    occluder_volume = 1.f;
    fade_volume += dt * 10.f * (bStopping ? -1.f : 1.f);
    volume_att = p_source.volume;
}
```

Replaced with an elapsed-time-driven, curve-aware computation:

```cpp
if (b2D)
{
    occluder_volume = 1.f;
    const float HALF_PI = 1.5707963267948966f;
    if (bStopping)
    {
        fade_out_elapsed += dt;
        float dur = (fade_out_duration_s > 0.f) ? fade_out_duration_s : 0.1f;
        float p = fade_out_elapsed / dur;
        clamp(p, 0.f, 1.f);
        if (p >= 1.f) fade_volume = 0.f;
        else fade_volume = (fade_out_curve == 1) ? cosf(p * HALF_PI) : (1.f - p);
    }
    else if (fade_in_duration_s > 0.f)
    {
        fade_in_elapsed += dt;
        float p = fade_in_elapsed / fade_in_duration_s;
        clamp(p, 0.f, 1.f);
        if (p >= 1.f) fade_volume = 1.f;
        else fade_volume = (fade_in_curve == 1) ? sinf(p * HALF_PI) : p;
    }
    else
    {
        fade_volume = 1.f;
    }
    volume_att = p_source.volume;
}
```

Using elapsed time (not per-frame accumulation) makes the fade duration
frame-rate independent — this was the actual fix for the click, which
happened more often under NPC/frametime-spike load. The 3D/distance-culling
branch further down in the same function was **not** touched.

### 6. `src/xrGame/script_sound.h`

Added to `CScriptSound`, next to `IC void StopDeffered();`:

```cpp
IC void SetFadeOut(float duration_s);
void SetFadeOut(float duration_s, int curve);
IC void SetFadeIn(float duration_s);
void SetFadeIn(float duration_s, int curve);
```

### 7. `src/xrGame/script_sound_inline.h` and `src/xrGame/script_sound.cpp`

`script_sound_inline.h` — added right after `CScriptSound::StopDeffered()`:

```cpp
IC void CScriptSound::SetFadeOut(float duration_s) { SetFadeOut(duration_s, 1); }
IC void CScriptSound::SetFadeIn(float duration_s)  { SetFadeIn(duration_s, 1); }
```

(the 1-arg overloads default to curve = 1, "equal-power", as the Lua-facing
convenience default)

`script_sound.cpp` — added right after `PlayNoFeedback(...)`:

```cpp
void CScriptSound::SetFadeOut(float duration_s, int curve) { m_sound.set_fade_out(duration_s, curve); }
void CScriptSound::SetFadeIn(float duration_s, int curve)  { m_sound.set_fade_in(duration_s, curve); }
```

### 8. `src/xrGame/script_sound_script.cpp`

Added Lua bindings right after `.def("stop_deffered", &CScriptSound::StopDeffered)`:

```cpp
.def("set_fade_out", (void (CScriptSound::*)(float))(&CScriptSound::SetFadeOut))
.def("set_fade_out", (void (CScriptSound::*)(float, int))(&CScriptSound::SetFadeOut))
.def("set_fade_in", (void (CScriptSound::*)(float))(&CScriptSound::SetFadeIn))
.def("set_fade_in", (void (CScriptSound::*)(float, int))(&CScriptSound::SetFadeIn))
```

This exposes to Lua:

```lua
sound_object:set_fade_out(duration_s)          -- curve defaults to equal-power
sound_object:set_fade_out(duration_s, curve)    -- curve: 0 = linear, 1 = equal-power
sound_object:set_fade_in(duration_s)
sound_object:set_fade_in(duration_s, curve)
```

## Build notes / gotchas for next time

- **Repo setup (current)**: this tree is `zoobi398/xray-monolith-bodycam`
  (`origin`), a GitHub fork of `asuparabekon/xray-monolith-bodycam`
  (`upstream`), itself a fork of `themrdemonized/xray-monolith`. The fade
  patch lives on branch `fade-system`, branched off `bodycam-mt`. To pull
  upstream updates: `git fetch upstream` then merge/rebase `bodycam-mt`
  (never edited directly) before rebasing `fade-system` on top. Set up via
  `gh repo fork asuparabekon/xray-monolith-bodycam --clone`.

- **Missing `optick-git` submodule**: the repo declares a git submodule at
  `src/3rd party/optick-git` (`.gitmodules` → `https://github.com/bombomby/optick.git`).
  A plain zip extract, or a clone without `git config --global core.longpaths true`
  set *before* `git submodule update --init --recursive`, leaves this folder
  empty or fails mid-checkout (optick's own repo has very long paths under
  `gui/` and `samples/`). Set the long-paths option globally once, then
  `git submodule update --init --recursive` works cleanly in one shot.

- **MAX_PATH (260 char) build failures**: MSBuild's C++ tasks
  (`GetOutOfDateItems`, `.tlog` files under `_build\intermediate\...`) do not
  tolerate the total path exceeding 260 characters, and fail with
  `MSB4018: ... exceeds the OS max path limit`. This build tree generates
  fairly long intermediate paths (project name + config name + `.tlog`
  filenames), so **keep the source tree at a short root path**, e.g.
  `C:\bodycam_build\`, not nested several folders deep under
  `Desktop\CLAUDE_PROJECTS\STALKER\...`.

- Build command used (from a short-path checkout):
  ```powershell
  & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
      "C:\bodycam_build\src\engine-vs2022.sln" /p:Configuration="DX11-AVX" /p:Platform="x64" /m
  ```
  Output: `C:\bodycam_build\_build\_game\bin_dbg\AnomalyDX11AVX.exe` (+ matching `.pdb`).

- Required VS2022 components (per this repo's own README): Desktop
  development with C++, current MSVC toolset, Windows SDK, MFC, ATL. Verified
  present: MSVC 14.44.35207, Windows SDK 10.0.26100.0, VC++ Redistributable
  v14.44.35211 (matches toolset — no runtime/compiler mismatch).

## Not part of this patch

The RPM-scaling (`rpm_scale_factor()`), burst-fire-mode handling, and the
`snd_shoot_start` soft-cutoff fade are **Lua-only** changes living in
`MOD_TRUE_CYCLIC/TRUE_CYCLIC_SOUNDS_V1_VERDATIM/gamedata/scripts/zzzzz_sound_loop_custom.script`
and the weapons' `.ltx` configs — they consume the `set_fade_out`/`set_fade_in`
API added here but require no further engine changes.
