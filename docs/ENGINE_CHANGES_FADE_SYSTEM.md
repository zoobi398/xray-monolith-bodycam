# Engine change: configurable per-emitter fade-out / fade-in for 2D sound sources

Status: implemented, not yet compiled/tested in-game (pending VS2022 build, config `DX11-AVX|x64`).

## Summary

Adds two new methods to the sound API, callable from both C++ and Lua, that let a script
control the fade-out duration/curve used by the existing `stop_deffered()` mechanism, and
add a symmetric fade-in for freshly played sounds (which the engine did not previously
support at all).

- `ref_sound::set_fade_out(float duration_s, int curve = 0)`
- `ref_sound::set_fade_in(float duration_s, int curve = 0)`
- Lua: `sound_object:set_fade_out(duration_s[, curve])`, `sound_object:set_fade_in(duration_s[, curve])`
- `curve`: `0` = linear, `1` = equal-power (cosine/sine). Lua single-argument overloads
  default to `1` (equal-power); the underlying engine default (for any code path that
  never calls these new methods) stays `0`/linear to exactly reproduce old behaviour.

**Scope**: 2D emitters only (`CSoundRender_Emitter::update_culling`, the `b2D` branch).
The 3D/distance-culling branch (AI/NPC positioned sounds, environmental audio) is
untouched. No behavioural change for any sound that does not explicitly call the new
setters — every existing call site in the game (menus, ambient, NPC voice lines, weapon
sounds played through the normal `HUD_SOUND_ITEM`/`CSoundPlayer` paths, etc.) is
unaffected.

## Motivation

The engine already supports a deferred/faded stop via `ref_sound::stop_deffered()` →
`CSound_emitter::stop(TRUE)` → `bStopping = TRUE`, ramped in
`CSoundRender_Emitter::update_culling()`. That ramp rate was hardcoded
(`fade_volume += dt * 10.f`, i.e. always ~100ms, linear), and there was no equivalent
fade-in for freshly played sounds (`fade_volume` is unconditionally set to `1.f` on every
new playback). This is being used by a gameplay mod that needs a crossfade between two
different sound instances (a continuous automatic-fire loop sample being stopped, and a
tail/reverb sample being started at the same instant) with duration matched to the
weapon's fire rate — a fixed 100ms is wrong for cycle times below ~150ms (i.e. most
automatic weapons), and produces audible artifacts (perceived "extra shot", clipped
tails, or a jarring full-volume attack on the new sound). Full details/rationale in
`AUDIO_CYCLIC_FADE_PROJECT_SUMMARY.md` in this repo.

## Files changed

### `src/xrSound/Sound.h`

Abstract interface `CSound_emitter` gains two pure virtuals, next to the existing
`stop(BOOL bDeffered)`:

```cpp
virtual void set_fade_out(float duration_s, int curve) = 0;
virtual void set_fade_in(float duration_s, int curve) = 0;
```

`ref_sound` (the public sound handle used throughout the engine/game code) gains matching
declarations and inline forwarders, following the exact same pattern as the existing
`stop()` / `stop_deffered()`:

```cpp
IC void set_fade_out(float duration_s, int curve = 0);
IC void set_fade_in(float duration_s, int curve = 0);
...
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

No-op (silently ignored) if the sound has no live feedback emitter, exactly like the
existing `set_volume`/`set_frequency`/etc. wrappers.

### `src/xrSound/SoundRender_Emitter.h`

`CSoundRender_Emitter` gains 6 new fields alongside the existing `fade_volume`:

```cpp
float fade_out_duration_s;
float fade_in_duration_s;
int   fade_out_curve; // 0 = linear, 1 = equal-power
int   fade_in_curve;  // 0 = linear, 1 = equal-power
float fade_out_elapsed;
float fade_in_elapsed;
```

and two new virtual method declarations next to `stop(BOOL bDeffered)`:

```cpp
virtual void set_fade_out(float duration_s, int curve);
virtual void set_fade_in(float duration_s, int curve);
```

### `src/xrSound/SoundRender_Emitter.cpp`

Constructor initializes the 6 new fields to defaults that reproduce the old, single
hardcoded behaviour exactly:

```cpp
fade_out_duration_s = 0.1f;   // matches the old fixed dt*10.f rate (1/10 = 0.1s)
fade_in_duration_s  = 0.f;    // 0 = fade-in disabled (old behaviour: instant full volume)
fade_out_curve      = 0;      // linear (matches the old linear ramp)
fade_in_curve       = 0;
fade_out_elapsed    = 0.f;
fade_in_elapsed     = 0.f;
```

### `src/xrSound/SoundRender_Emitter_StartStop.cpp`

- `start()`: resets the 6 fields to the same defaults on every (re)start. Needed because
  `CSoundRender_Emitter` instances can be pooled/reused across unrelated playbacks; this
  guarantees no stale fade configuration leaks from a previous sound into a new one that
  never opts in.
- `stop(BOOL bDeffered)`: when deferred, additionally resets `fade_out_elapsed = 0.f` so
  the fade-out ramp starts counting from the moment the stop was actually requested:

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

- Two new method bodies:

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
        // Force silence now; update_culling() ramps fade_volume back up to 1.0
        // over fade_in_duration_s on subsequent frames.
        fade_in_elapsed = 0.f;
        fade_volume = 0.f;
    }
}
```

`set_fade_out()` only stores configuration — it takes effect the next time
`stop(TRUE)`/`stop_deffered()` is called. `set_fade_in()` takes effect immediately
(intended to be called by a script right after `play()`).

### `src/xrSound/SoundRender_Emitter_FSM.cpp`

`CSoundRender_Emitter::update_culling()`, `b2D` branch. Replaces:

```cpp
if (b2D)
{
    occluder_volume = 1.f;
    fade_volume += dt * 10.f * (bStopping ? -1.f : 1.f);
    volume_att = p_source.volume;
}
```

with an elapsed-time-driven computation (direct assignment each frame rather than
accumulation, so duration/curve are exact and independent of frame rate/missed frames):

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

For any emitter that never calls `set_fade_out`/`set_fade_in`, this produces bit-for-bit
equivalent perceptual behaviour to the old code: `fade_out_duration_s` defaults to
`0.1f` and `fade_out_curve` to linear, so the fade-out ramp is identical in shape and
duration to the original `dt*10.f` accumulation (just computed from elapsed time instead
of accumulated per-frame, which also fixes a latent, unrelated issue where the old
accumulation could overshoot/undershoot slightly under irregular frame timing).
`fade_in_duration_s` defaults to `0.f`, taking the `else` branch and forcing
`fade_volume = 1.f` every frame — identical to a sound that was never ramping in the
first place.

The 3D branch (distance-culling fade, `else` block further down in the same function) is
untouched.

### `src/xrGame/script_sound.h`, `script_sound_inline.h`, `script_sound.cpp`, `script_sound_script.cpp`

Standard Lua-binding boilerplate, following the exact existing pattern used for
`Play(object)` / `Play(object, delay)` / `Play(object, delay, flags)`:

- `script_sound.h`: declares `SetFadeOut(float)` / `SetFadeIn(float)` (inline
  convenience overloads) and `SetFadeOut(float, int)` / `SetFadeIn(float, int)`
  (out-of-line, implemented in `script_sound.cpp`).
- `script_sound_inline.h`: the 1-argument overloads forward to the 2-argument ones with
  `curve = 1` (equal-power) as the Lua-facing default:

```cpp
IC void CScriptSound::SetFadeOut(float duration_s) { SetFadeOut(duration_s, 1); }
IC void CScriptSound::SetFadeIn(float duration_s)  { SetFadeIn(duration_s, 1); }
```

- `script_sound.cpp`: the 2-argument overloads simply forward to `m_sound.set_fade_out`/
  `set_fade_in`.
- `script_sound_script.cpp`: registers both overloads of each method with luabind,
  mirroring the existing `play` registration style:

```cpp
.def("set_fade_out", (void (CScriptSound::*)(float))(&CScriptSound::SetFadeOut))
.def("set_fade_out", (void (CScriptSound::*)(float, int))(&CScriptSound::SetFadeOut))
.def("set_fade_in", (void (CScriptSound::*)(float))(&CScriptSound::SetFadeIn))
.def("set_fade_in", (void (CScriptSound::*)(float, int))(&CScriptSound::SetFadeIn))
```

## Backward compatibility

No default value changes for any existing sound. Verified by construction: every new
field's default reproduces the exact previous constant/behaviour, and the new code paths
in `update_culling()` are only reachable through explicit calls to the new setters.
Nothing in `xrEngine`, `xrGame`, or any other existing script calls `set_fade_out`/
`set_fade_in` except the gameplay mod this was built for
(`MOD_TRUE_CYCLIC/TRUE_CYCLIC_SOUNDS_V1_VERDATIM/gamedata/scripts/zzzzz_sound_loop_custom.script`).

## Known limitations / possible follow-ups

- 3D emitters (positioned/AI sounds) do not get configurable fade duration/curve; only
  the existing fixed distance-culling fade applies to them. Extending this would mean
  touching the `else` branch of `update_culling()` and deciding how it interacts with
  occlusion/culling-driven fades — out of scope for this change.
- No fade "shape" beyond linear and equal-power (e.g. no exponential-decay-with-tail or
  custom curve callback). Equal-power was chosen because it's the standard choice for
  audio crossfades and was sufficient for the target use case.
- `set_fade_in` takes effect only if called before the *next* `update()` tick for that
  emitter; in practice this means "call it right after `play()`, synchronously" — which
  is how the one current caller uses it. Not an issue in practice given the game's single
  Lua VM / single script thread model, but worth knowing if reused elsewhere.

## Testing status

Not yet compiled or run in-game as of this writing. Companion mod-side changes are in
`MOD_TRUE_CYCLIC/TRUE_CYCLIC_SOUNDS_V1_VERDATIM/gamedata/` (script + FG42/G3 configs) in
the same repo, exercising this new API. See `CHANGELOG.md` for the full list of changes
across both the engine and the mod.
