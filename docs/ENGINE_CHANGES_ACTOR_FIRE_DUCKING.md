# Engine change: actor-fire priority ducking on NPC gunshot voices ("snd_duck_mode 1")

Status: implemented and compiled (`DX11-AVX|x64`, `xrEngine` target via `engine-vs2022.sln`, 0 errors).
Deployed, not yet tested in-game.

## Motivation

After the sound occlusion rework (`05_ENGINE_CHANGES_SOUND_OCCLUSION_PHASE1_2.md`) started giving NPC
gunshots a consistently accurate, full-loudness result in genuinely clear line of sight (instead of the
old system's flickery, occasionally-too-quiet result), the user reported that firing back at an NPC who
is also firing, face to face, made their own shot sound quieter than normal. Root cause (discussed and
confirmed, not guessed): OpenAL Soft applies an `output-limiter` on the final mixed output by default for
non-float32 sample types (`F:\ANOMALY_forGAMMA_V095\bin\alsoft.ini` has `sample-type = int32`, and
`output-limiter` is left commented out, so the compiled-in default -- on -- applies). When a loud, unoccluded
NPC shot and the actor's own shot sum close to the output ceiling, the limiter compresses the whole mix,
and the actor's own shot is what's audibly squashed.

Rather than permanently lowering NPC weapon loudness (which would affect every situation, including pure
observation with no overlap, for no reason), this adds a temporary, event-triggered gain reduction on NPC
gunshot voices specifically while the actor is firing -- the standard "sidechain/priority ducking"
technique several modern FPS titles use for the same reason. Full design discussion (routing, envelope
shape, performance, why not a real OpenAL "bus") happened in chat before implementation; summarized below.

## Summary

- **Trigger**: `CActor::on_weapon_shot_start()` (the actor's own shot only -- NPCs firing does not
  trigger this). Not a gameplay/script event; a direct C++ call into the sound system.
- **Target**: 3D emitters with `occ_profile == 1` (gunshot/explosion-profile, the same classification
  `get_occlusion_ex` already uses -- see Phase 1/2 doc). The actor's own weapon sound is never a possible
  target: it's played 2D (`b2D`), which never reaches the branch where ducking is computed, so no
  explicit self-exclusion check is needed.
- **Loudness-weighted**: the duck depth for a given NPC voice scales with that voice's own current
  loudness (`volume_att * occluder_volume`, both already computed this frame for other purposes) -- an
  NPC shooting from 100m (already quiet) is barely touched; one at 5m (near full gain) gets the full
  configured duck. Answers the user's own concern directly, at no extra cost (no new distance
  calculation).
- **Click-free**: reuses `volume_lerp()` (`SoundRender_Emitter_FSM.cpp:364`), the exact same rate-limited
  smoothing primitive already proven (via the occlusion work) not to click, instead of stepping the gain
  directly. Attack (engaging the duck) and release (letting go) use two different rates -- fast attack,
  slower release -- so full-auto fire holds the duck continuously instead of pumping per shot.
- **Routing**: unlike occlusion (which was deliberately moved off `AL_GAIN` onto the two EFX filters to
  decouple the direct/reverb-send paths -- see Phase 1/2's "Update (30/09)"), ducking multiplies directly
  into `smooth_volume` (which drives `AL_GAIN`). This is intentional, not an oversight: auditory masking
  is a property of the listener, not of the sound's propagation path, so ducking should reduce the direct
  signal AND the reverb send equally -- and `AL_GAIN` already scales both proportionally in OpenAL, so no
  separate filter plumbing was needed.
- **Performance**: no ray casts, no geometry queries -- pure scalar arithmetic on data already computed
  this frame (`volume_att`, `occluder_volume`) plus one more `volume_lerp()` call (the same primitive
  occlusion already calls three times per emitter). Runs inside `update_culling()`, which every active 3D
  emitter already executes every frame regardless; no new iteration/dispatch is added. Also confirmed
  (by reading `x_ray.cpp`/`EngineThreading.cpp`) that sound processing already runs on a secondary
  parallel task (`seqFrameMT`, gated by `mtSound`, on by default per `defines.cpp`) rather than the main
  thread -- this cost doesn't compete with render/game-logic budget either way.
- **Off by default, byte-identical otherwise**: `snd_duck_mode` defaults to `0`. `duck_gain` is
  initialized to `1.f` and never modified unless `snd_duck_mode` is on and the voice is gunshot-profile --
  the `* duck_gain` multiply added to `smooth_volume` is a no-op for every 2D sound and every non-gunshot
  3D sound regardless of the mode.

## Design choices worth flagging

1. **Why the trigger is a direct C++ call, not a gameplay event/RTPC.** `xrSound` has no concept of "the
   actor fired" on its own -- that's a gameplay fact, only known in `xrGame`. `CSound_manager_interface`
   (the existing bridge `xrGame` already uses for every sound operation, `::Sound->...`) gained one more
   pure virtual, `on_actor_weapon_shot()`, called once from `CActor::on_weapon_shot_start()`
   (`Actor_Weapon.cpp`) -- not from `CAI_Stalker::on_weapon_shot_start()` (NPCs have their own override of
   the same virtual, deliberately untouched, so NPC-vs-NPC fire never triggers a duck).
2. **Why the timestamp is recorded on the sound system's own clock, not a shared engine clock.**
   `CSoundRender_Core` already runs its own `CTimer Timer` / `fTimer_Value` (independent of
   `Device.fTimeGlobal`), and the existing occlusion cadence-gating (`occ_next_update`) already reads it.
   `on_actor_weapon_shot()` just does `m_actor_last_shot_time = fTimer_Value` -- recorded and later read
   on the same clock, so no cross-module clock synchronization is needed at all.
3. **Why the envelope is a time-window flip + `volume_lerp`, not a full ADSR state machine.** The raw
   target is just: ducked value while `now - last_shot_time < snd_duck_hold_ms`, else `1.0`. Every new
   shot during a burst pushes `last_shot_time` forward, which keeps the window (and therefore the ducked
   target) alive continuously through full-auto fire -- no per-shot state, no re-triggering logic needed.
   `volume_lerp`, called with a fast rate while approaching the ducked value and a slower rate while
   returning to `1.0`, turns that step function into the actual smooth attack/release heard in-game. This
   mirrors exactly how `occ_target_gain` (a value that itself changes in discrete steps, on its own
   cadence) is already turned into a smooth `occluder_volume` elsewhere in the same function.
4. **Why this isn't a real audio "bus".** OpenAL has no submix/bus/sidechain-compressor concept the way
   Wwise or FMOD do. This is a lightweight, event-driven approximation (game logic decides when ducking
   applies; no literal PCM/RMS analysis of the mixed signal), which is how most shipped games implement
   this feature in practice regardless of audio middleware. Scoped narrowly (gunshot-profile NPC voices
   only, actor's own fire as the only trigger) rather than built as a generic reusable bus graph, since
   that's the only concrete need so far -- extending this later to duck other sound categories would reuse
   the same envelope and `volume_lerp` pattern, not require a new architecture.
5. **Why loudness-weighting uses `volume_att * occluder_volume`, not a fresh distance calculation.** Both
   are already computed earlier in the very same `update_culling()` call for this emitter, for unrelated
   reasons (distance falloff and occlusion respectively) -- reusing them costs nothing and already reflects
   both "how far" and "how occluded" this specific NPC shot currently is.

## Update (30/09) -- default strength grounded in Phase 0's own measured number, TRUE_CYCLIC_SOUNDS verified

Two follow-up questions from the user, both resolved before shipping the final default:

1. **Is `snd_duck_strength`'s default well-chosen, or arbitrary?** Originally shipped at `0.45`
   (~-5.2dB) with no strong justification. The user proposed a cleaner anchor: Phase 0's own measured
   baseline (`04_ENGINE_CHANGES_SOUND_OCCLUSION_PHASE0.md`) found the *original* occlusion system's single
   ray was "blocked" (applying its flat -8dB) **78.1% of the time on average, in this exact Garbage
   hangar scene** -- i.e. in cluttered indoor combat, NPC gunshots used to be quasi-permanently ~8dB
   quieter than they are now that occlusion is accurate. Ducking "gives back" that same order of
   magnitude, but ONLY during the narrow window the actor is actually firing -- not constantly like the
   old system's false positives did. Changed the default from `0.45` to **`0.55`** (~-7dB at full
   `loudness_weight`, the middle of the 6-8dB range), in both `SoundRender_Core.cpp` and the MCM page's
   `def`. A player firing themselves at the moment of the comparison is itself a realistic justification
   independent of the occlusion history -- a real gunshot at your own ear measurably masks simultaneous
   perception of other sounds, so this isn't purely a compensatory hack.
2. **Does `TRUE_CYCLIC_SOUNDS_V1_VERDATIM` (the user's own custom weapon-audio mod) break the trigger?**
   Verified, not assumed: the mod hooks the Lua callback `actor_on_weapon_fired`
   (`GameObject::eOnWeaponFired`), which `WeaponMagazined.cpp:895`'s `CWeaponMagazined::OnShot()` dispatches
   as a sibling call to `AddShotEffector()` (the function that leads to `on_weapon_shot_start()`, this
   feature's trigger) -- both unconditional, both fired for every real shot regardless of which sound
   system is configured for that weapon. `PlaySoundShot()`'s sound selection is the only thing
   `TRUE_CYCLIC_SOUNDS` changes; the camera/recoil/ducking trigger chain is untouched. No compatibility
   risk.

## Files changed

### `src/xrSound/Sound.h`

New console-bound globals, next to the occlusion ones:

```cpp
XRSOUND_API extern int psSoundDuckMode;           // snd_duck_mode: 0 off (default), 1 on
XRSOUND_API extern float psSoundDuckStrength;     // snd_duck_strength: 0-1, max gain cut on a full-loudness NPC shot
XRSOUND_API extern float psSoundDuckHoldMs;       // snd_duck_hold_ms: how long the duck stays engaged after the actor's last shot
XRSOUND_API extern float psSoundDuckAttackRate;   // snd_duck_attack_rate: gain units/s approaching the ducked target
XRSOUND_API extern float psSoundDuckReleaseRate;  // snd_duck_release_rate: gain units/s releasing back to 1.0
```

One new pure virtual on `CSound_manager_interface`:

```cpp
// Called once from CActor::on_weapon_shot_start(). No-op at snd_duck_mode 0.
virtual void on_actor_weapon_shot() = 0;
```

### `src/xrSound/SoundRender_Core.h`

```cpp
float m_actor_last_shot_time = -1000.f; // far in the past -- nothing is ducked before the first shot ever fires
virtual void on_actor_weapon_shot() override { m_actor_last_shot_time = fTimer_Value; }
```

### `src/xrSound/SoundRender_Core.cpp`

Global definitions: `psSoundDuckMode = 0`, `psSoundDuckStrength = 0.45f`, `psSoundDuckHoldMs = 120.f`,
`psSoundDuckAttackRate = 18.f`, `psSoundDuckReleaseRate = 3.f`. At the defaults: a full-loudness NPC shot
is cut to `0.55` gain (~-5.2dB) within ~25ms of the actor firing, held through a sustained burst (any
weapon cycling faster than 120ms/shot keeps re-arming the hold window), and released back to `1.0` over
~150ms after the actor's last shot -- conservative starting points, meant to be tuned by feel via the new
MCM page, not physically derived.

### `src/xrSound/SoundRender_Emitter.h`

```cpp
float duck_gain; // separate from occluder_volume -- see design note 4 for why it isn't folded in
```

### `src/xrSound/SoundRender_Emitter.cpp` / `SoundRender_Emitter_StartStop.cpp`

Constructor and `start()` both initialize/reset `duck_gain = 1.f` (the same defensive reset pattern
`occ_profile`/`occ_next_update`/the fade fields already use, guarding against a reused emitter starting a
new, unrelated sound already ducked from whatever the previous sound was doing).

### `src/xrGame/Actor_Weapon.cpp`

One line added at the top of `CActor::on_weapon_shot_start()`:

```cpp
::Sound->on_actor_weapon_shot();
```

### `src/xrSound/SoundRender_Emitter_FSM.cpp` -- the actual mechanism

Appended after the existing occlusion if/else chain in `update_culling()`'s 3D branch (runs regardless of
`snd_occlusion_mode`, since ducking is an independent layer on top of whatever occlusion already
computed):

```cpp
if (psSoundDuckMode && occ_profile == 1)
{
    const float loudness_weight = _min(_max(volume_att * occluder_volume, 0.f), 1.f);
    const float since_last_shot_ms = (SoundRender->fTimer_Value - SoundRender->m_actor_last_shot_time) * 1000.f;
    const bool duck_active = since_last_shot_ms < psSoundDuckHoldMs;
    const float duck_target = duck_active ? (1.f - psSoundDuckStrength * loudness_weight) : 1.f;
    const float rate = (duck_target < duck_gain) ? psSoundDuckAttackRate : psSoundDuckReleaseRate;
    volume_lerp(duck_gain, duck_target, rate, dt);
    clamp(duck_gain, 0.f, 1.f);
}
else
{
    duck_gain = 1.f;
}
```

And the existing `smooth_volume` formula (drives `AL_GAIN`) gained one more multiplicative term:

```cpp
smooth_volume = (p_source.base_volume * volume_att * (...effects/music volume...) * occ_for_gain * fade_volume * duck_gain);
```

### `src/xrEngine/xr_ioc_cmd.cpp`

```cpp
CMD4(CCC_Integer, "snd_duck_mode", &psSoundDuckMode, 0, 1);
CMD4(CCC_Float, "snd_duck_strength", &psSoundDuckStrength, 0.f, 1.f);
CMD4(CCC_Float, "snd_duck_hold_ms", &psSoundDuckHoldMs, 0.f, 1000.f);
CMD4(CCC_Float, "snd_duck_attack_rate", &psSoundDuckAttackRate, 0.5f, 100.f);
CMD4(CCC_Float, "snd_duck_release_rate", &psSoundDuckReleaseRate, 0.5f, 100.f);
```

### `gamedata/scripts/options_modded_exes_sound_duck.script` (new) + `options_modded_exes_gameplay.script`
+ `st_bodycam_mcm.xml`

A new "Actor Fire Ducking" MCM sub-page (Options -> Modded EXEs -> Gameplay -> Actor Fire Ducking, right
after Sound Occlusion), same `track`/`list_bool`-bound-to-console-command idiom as the sound occlusion
page. Wired into `options_modded_exes_gameplay.script`'s `GROUP` table -- learned from the sound occlusion
page shipping once without this step and being invisible in the menu until caught and fixed.

## Backward compatibility

`snd_duck_mode` defaults to `0`. `duck_gain` starts at `1.f` and the only code path that ever changes it
is gated on `psSoundDuckMode && occ_profile == 1`; the `else` branch forces it back to `1.f` every frame
otherwise. The `* duck_gain` term added to `smooth_volume` is therefore a no-op multiply for every sound
in the game unless a player explicitly turns ducking on. No existing behaviour changes.

## Known limitations / possible follow-ups

- Loudness-weighting uses the NPC voice's own occlusion-adjusted gain, not a measurement of how loud the
  actor's own shot actually is right now (weapon-dependent: a suppressed weapon is much quieter than an
  unsuppressed one). `psSoundDuckStrength` is a single global value regardless of which weapon the actor
  is firing -- a future refinement could read the actor's current weapon's own loudness and scale the duck
  accordingly, not implemented here (no clear need identified yet).
- Only gunshot/explosion-profile (`occ_profile == 1`) 3D voices are ever ducked. Deliberately excludes
  everything else (footsteps, voices, ambience, looped sounds) per the user's own scoping -- revisit only
  if a concrete case for ducking something else shows up.
- The duck depth/timing defaults (`0.45` / `120ms` / `18` / `3`) are starting points for in-game tuning,
  not derived from any reference. Expect to adjust via the new MCM sliders after the first test.

## Testing status

Compiled clean, deployed (binary + the 3 gamedata files, both to the engine repo's own gamedata and to
`F:\ANOMALY_forGAMMA_V095\gamedata`). Not yet tested in-game.
