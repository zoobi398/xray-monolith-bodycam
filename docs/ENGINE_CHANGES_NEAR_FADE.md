# Engine change: near-fade for layered NPC gunshot sounds (snd_X_layer)

Status: implemented and compiled (`DX11-AVX|x64`, `xrEngine` target, 0 errors), not yet tested in-game.

## Summary

Adds two optional `.ltx` fields, `fade_start` and `fade_full` (metres), to any `snd_N_layer`/
`snd_N_layerK` line in a layered sound collection (`HUD_SOUND_COLLECTION_LAYERED`, used by
`snd_shoot`-style NPC weapon sound sections). Below `fade_start`, that layer/variant simply isn't
played -- no emitter, no OpenAL voice, no AI sound-perception event. Between `fade_start` and
`fade_full` its volume ramps linearly up to normal. Fully opt-in: absent on a line (or on an
unpatched exe) means exactly today's behaviour.

```ini
; path, volume, delay, fade_start, fade_full
snd_4_layer  = weapons\m1a\4_far_1, 1, 0, 40, 80
snd_4_layer1 = weapons\m1a\4_far_2               ; inherits 40/80 from snd_4_layer
snd_3_layer  = weapons\m1a\3_medium_1, 1, 0, 15, 30
snd_1_layer  = weapons\m1a\1_very_close_1        ; no fade fields -> unchanged
```

## Motivation

A layered NPC gunshot collection (e.g. `snd_1_layer` = very close, `snd_2_layer` = close,
`snd_3_layer` = medium, `snd_4_layer` = far) plays **every layer on every shot**, each shaped only by
its own OGG metadata (`min_distance`/`max_distance`/`base_sound_volume`). That metadata can only make a
layer *quieter with distance*; it has no way to make a layer quieter *up close*. A "far" layer (large
`min_distance`, typically 80-150m) is therefore always at full, unattenuated volume for any NPC closer
than its `min_distance` -- including a NPC standing 10m away. It stacks loudness and adds a "distant
gunfire" character (filtering, echo) onto a point-blank shot, and it burns an OpenAL voice for a sound
whose presence at that distance is actively wrong.

The engine already computes listener-to-source distance every frame per emitter (and again inside
OpenAL) and already has a "too far -> don't play" cutoff (`max_distance`). There was no symmetric
"too near -> don't play" cutoff. This change adds one, at the one point in the existing call chain
where a per-layer distance check is cheap and can prevent the emitter from ever being created: not
`update_culling()` (every frame, per already-live emitter), but the one-time decision at shot time in
`HUD_SOUND_ITEM::PlaySound()`.

### Call chain (verified against this fork's source, not assumed from the ltx)

| Step | Code | What happens |
|---|---|---|
| 1. Shot | `HUD_SOUND_COLLECTION_LAYERED::PlaySound` -- `HudSound.cpp` | Runs the `_G.COnBeforePlayHudSound` Lua hook (used by Spatial Audio Rework for indoor variants), then loops over every layer sharing the alias. |
| 2. Layer | `HUD_SOUND_ITEM::PlaySound` -- `HudSound.cpp` | Picks a random variant, then `play_no_feedback(...)` (or `play_at_pos` if exclusive). No distance check before this change. |
| 3. Emitter | `CSoundRender_Core::play_no_feedback` -- `SoundRender_Core.cpp` | Creates the emitter; `p_source.volume` is set from the passed `volume_mult`. A stereo file is forced to 2D (no distance attenuation at all). |
| 4. Every frame | `CSoundRender_Emitter::update_culling` -- `SoundRender_Emitter_FSM.cpp` | Computes `dist`; beyond `max_distance` the emitter is culled (no voice); otherwise a distance ramp scales the volume. |
| 5. AI hearing | `_sound_event()` -> `IGame_Level::SoundEvent_Register(S, range)` -- `IGame_Level.cpp` | Only invoked for sounds that actually got a real feedback emitter (`S->feedback` must be non-null). **A layer this patch prevents from ever playing also never registers for AI sound perception.** |

Step 5 is why `fade_start`/`fade_full` must never be set on the closest layer of a collection: doing so
would make that NPC's gunshot inaudible to nearby AI, not just to the player.

## Design considered but not used

- **Data-only (lower the far layer's BSV):** reduces the excess loudness but the layer still plays up
  close (wrong tonal character, still costs a voice) -- and it also quietens the layer at the distance
  it's supposed to be heard at. Doesn't solve the actual problem.
- **Lua script (`COnBeforePlayHudSound`) swapping the whole collection by distance tier:** works, but
  replaces the *entire* collection in discrete steps (audible pops at thresholds), needs duplicated ltx
  sections per weapon/indoor variant, and **conflicts with Spatial Audio Rework**, which writes the same
  `result.section`/`result.line` fields through the same callback (`SendScriptCallback`) -- last script
  registered wins, so the two features can silently fight over the same hook.
- **Same fix inside `update_culling()` (xrSound) instead of at shot time (xrGame):** continuous
  re-evaluation if the NPC moves during a sustained burst, and zero extra per-shot cost -- but requires
  threading two new parameters through `CSound_params`/`ref_sound`/`play_no_feedback` down into
  `xrSound`, a materially larger/more invasive change, for a benefit (mid-burst distance changes) that
  doesn't matter for gunshots (they're one-shot, not looped). Rejected in favour of the simpler,
  self-contained `xrGame`-only change below; noted here in case a future looped-sound use case needs it.

## Files changed

Only `src/xrGame/HudSound.h` and `src/xrGame/HudSound.cpp`.

### `src/xrGame/HudSound.h`

`SSnd` (the per-variant struct on `HUD_SOUND_ITEM`) gains 3 fields, and the item gains one new static
method declared right after `SSnd` (so the type is already complete at that point):

```cpp
struct SSnd
{
    ref_sound snd;
    float delay;
    float volume;

    // Near-fade (world sounds only): below fade_start (m) this variant isn't played at all (no
    // emitter created); it ramps linearly to full volume by fade_full (m). fade_set is false
    // (both ignored) unless the .ltx line carries the optional 4th field.
    float fade_start = 0.f;
    float fade_full = 0.f;
    bool fade_set = false;
};

// Reads the optional near-fade fields (items 3/4 of the ltx line, 0-based) into s.fade_start/
// fade_full/fade_set. No-op (fade_set stays false) if the line doesn't carry them.
static void LoadNearFade(LPCSTR section, LPCSTR line, SSnd& s);
```

### `src/xrGame/HudSound.cpp` -- loading

A small static helper (top of file, next to the other file-scope statics):

```cpp
// Gain factor for a world sound heard at distance 'dist', ramping linearly from 0 at 'start' to 1 at
// 'full'. 'full' <= 'start' means a hard on/off cut at 'start' instead of a ramp.
static float NearFadeFactor(float dist, float start, float full)
{
    if (full <= start)
        return dist >= start ? 1.f : 0.f;
    if (dist <= start)
        return 0.f;
    if (dist >= full)
        return 1.f;
    return (dist - start) / (full - start);
}
```

`LoadNearFade`, reading items 3/4 of the same ltx line `LoadSound` already parses items 0-2 from
(reuses the same `_GetItemCount`/`_GetItem`/`atof` idiom already used a few lines above it in this
file):

```cpp
void HUD_SOUND_ITEM::LoadNearFade(LPCSTR section, LPCSTR line, SSnd& s)
{
    LPCSTR str = pSettings->r_string(section, line);
    string256 buf;
    const int count = _GetItemCount(str);

    if (count > 3)
    {
        _GetItem(str, 3, buf);
        if (xr_strlen(buf) > 0)
        {
            s.fade_start = (float)atof(buf);
            s.fade_set = s.fade_start > 0.f;
        }
    }
    if (count > 4)
    {
        _GetItem(str, 4, buf);
        if (xr_strlen(buf) > 0)
            s.fade_full = (float)atof(buf);
    }
    if (s.fade_set && s.fade_full < s.fade_start)
        s.fade_full = s.fade_start; // hard cut at fade_start, no ramp
}
```

Wired into the per-variant loading loop, plus an inheritance pass so a layer only has to write
`fade_start`/`fade_full` once (on `snd_N_layer`), not on every `snd_N_layerK` variant:

```cpp
void HUD_SOUND_ITEM::LoadSound(LPCSTR section, LPCSTR line, HUD_SOUND_ITEM& hud_snd, int type)
{
    hud_snd.m_activeSnd = NULL;
    hud_snd.sounds.clear();

    string256 sound_line;
    xr_strcpy(sound_line, line);
    int k = 0;
    while (pSettings->line_exist(section, sound_line))
    {
        hud_snd.sounds.push_back(SSnd());
        SSnd& s = hud_snd.sounds.back();

        LoadSound(section, sound_line, s.snd, type, &s.volume, &s.delay);
        LoadNearFade(section, sound_line, s);
        xr_sprintf(sound_line, "%s%d", line, ++k);
    }

    // Variants without their own near-fade fields inherit the first line's (snd_N_layer).
    if (!hud_snd.sounds.empty() && hud_snd.sounds.front().fade_set)
    {
        const SSnd& first = hud_snd.sounds.front();
        for (SSnd& variant : hud_snd.sounds)
        {
            if (!variant.fade_set)
            {
                variant.fade_start = first.fade_start;
                variant.fade_full = first.fade_full;
                variant.fade_set = true;
            }
        }
    }
}
```

### `src/xrGame/HudSound.cpp` -- applying it at shot time

In `HUD_SOUND_ITEM::PlaySound()`, right after the variant is picked (`hud_snd.m_activeSnd =
&hud_snd.sounds[index];`) and before the `m_b_exclusive` branch that actually starts playback:

```cpp
hud_snd.m_activeSnd = &hud_snd.sounds[index];

if (!b_hud_mode && hud_snd.m_activeSnd->fade_set)
{
    const float dist = ::Sound->listener_position().distance_to(position);
    const float k = NearFadeFactor(dist, hud_snd.m_activeSnd->fade_start, hud_snd.m_activeSnd->fade_full);
    if (g_near_fade_debug_log)
    {
        Msg("* near-fade %s dist=%.1f start=%.1f full=%.1f k=%.2f%s", hud_snd.m_alias.c_str(), dist,
            hud_snd.m_activeSnd->fade_start, hud_snd.m_activeSnd->fade_full, k, k <= EPS_S ? " SKIPPED" : "");
    }
    if (k <= EPS_S)
    {
        hud_snd.m_activeSnd = NULL;
        return;
    }
    volume_mult *= k;
}

if (hud_snd.m_b_exclusive)
{
    ...
```

`volume_mult` is already threaded through to both `play_no_feedback(..., &volume_mult, ...)` and the
trailing `set_volume(... * volume_mult)` call later in the same function, so multiplying it here before
either of those runs is sufficient -- no other call site needed touching.

`b_hud_mode` is the function's existing parameter (true for `sm_2D` player-HUD sounds); gating on
`!b_hud_mode` means the player's own weapon sounds are never affected, matching the spec.

`EPS_S` and `::Sound->listener_position()` are both already used elsewhere in this exact file/module
(`EPS_S` in `HUD_SOUND_COLLECTION_LAYERED::PlaySound`, `Sound->` throughout `xrGame` for sound
creation/positioning) -- no new includes were needed.

## Backward compatibility

- Absent `fade_start`/`fade_full` on a line -> `fade_set` stays `false` -> the new block in
  `PlaySound()` never executes -> byte-identical to the old code path for that variant.
- An unpatched/vanilla exe reading a `.ltx` with the new 4th/5th fields simply never reads past item 2
  (its own `LoadSound` only looks at items 0-2) -- no crash, no behaviour change, the fields are inert.
- Nothing else calls `HUD_SOUND_ITEM`/`HUD_SOUND_COLLECTION_LAYERED` differently; detectors, UI sounds,
  `CarWeapon`, etc. never set the new fields, so `fade_set` is always `false` for them.

## Points of attention

1. **Never set fade on the closest layer of a collection.** Per the call-chain table above, a layer
   this patch prevents from playing also never fires `SoundEvent_Register`, i.e. AI stops hearing that
   specific layer's contribution. Keep at least one always-on, unfaded layer (the nearest-distance one)
   so NPCs always register the shot.
2. **`volume_mult` is a ceiling further downstream (`update_culling`), not a pure multiplier**, but at
   shot time here in `HudSound.cpp` the distance-based ramp in `update_culling` is effectively ~1 for a
   layer actually meant to be heard at this range, so multiplying `volume_mult` by `k` here behaves as
   the intended multiplier in practice.
3. **Listener reference**: `::Sound->listener_position()`, the same position `update_culling` itself
   uses -- consistent with the rest of the engine's distance-based audio logic, not a separate camera
   read.
4. **Stereo files are forced to 2D** (`play_no_feedback`, unrelated pre-existing behaviour) -- near-fade
   still works for them since the distance is computed here in `HudSound.cpp` from the world position
   argument, before the stereo-forces-2D branch inside `xrSound` ever runs. NPC gunshot layers should be
   mono regardless, for normal distance attenuation to work at all.
5. **SAR (Spatial Audio Rework) indoor replacement**: its replacement collection is loaded through the
   same `LoadSound`, so its lines get near-fade too if authored with the extra fields. No conflict --
   the Lua hook runs first and only swaps which collection/alias plays; it doesn't touch the new fields.
6. **Curve**: linear gain ramp, chosen for simplicity/predictability. An equal-power curve
   (`sinf(k * PI_DIV_2)`) was considered but not used, to keep this symmetrical with how
   `update_culling`'s own distance ramp already works (also linear). If the curve changes later, the
   Python "LayerSim" theoretical-render tool needs the same formula to stay accurate.

## Test plan

1. Build (`DX11-AVX|x64`, `xrEngine`), install the resulting exe.
2. Author a weapon's `snd_shoot` layers with fade fields, e.g. `4_far : 40, 80`, `3_medium : 15, 30`.
   A ready-to-use example patch targeting `wpn_m1a_new_snd_shoot_npc`/`_indoor` (TRUE_CYCLIC_SOUNDS_
   V1_VERDATIM's M1A) ships in `NEAR_FADE_TEST_MOD`.
3. Toggle the per-shot debug dump with the console command `g_near_fade_debug_log 1` (same pattern as
   `g_insurgency_recoil_debug_log` -- a `BOOL`, off by default, registered in `console_commands.cpp`,
   defined in `HudSound.cpp`). Logs `near-fade <alias> dist=.. start=.. full=.. k=..` (plus ` SKIPPED`
   when the layer doesn't play) every time a faded layer is evaluated -- no rebuild needed to enable or
   disable it, unlike a hardcoded temporary `Msg()`.
4. In-game, free camera, NPC firing at varying distances:
   - ~10m: far and medium layers log `SKIPPED`.
   - ~60m: far layer logs `k~0.5`.
   - ~100m: far layer logs `k=1`.
5. Non-regression: player's own (HUD) shots unchanged; weapons without fade fields unchanged; SAR indoor
   replacement still works; a vanilla/unpatched exe with the same `.ltx` doesn't crash and simply ignores
   the fields.
6. Load: a firefight with several NPCs in a burst should show fewer rendered voices at short range (via
   the engine's own `_rendered`/`_simulated` sound stats console output).

## Known limitations / possible follow-ups

- No per-weapon-section default or global fallback -- every layer that wants near-fade needs its own
  (or its layer-root's) `fade_start`/`fade_full` written explicitly. Fine for the targeted use case
  (hand-tuned `snd_shoot` collections), but there's no "apply to every far layer automatically" shortcut.
- The curve is a fixed linear ramp; if a different shape is wanted later, `NearFadeFactor` is the single
  place to change, but the Python LayerSim tool's theoretical render must be updated to match or it will
  no longer predict in-game behaviour accurately.
- Not wired into `update_culling`/`xrSound` (Design considered but not used, above) -- a NPC that starts
  a burst just past `fade_full` and then closes distance mid-burst won't have that specific already-
  playing layer instance re-evaluated; each new shot re-evaluates fresh, which is sufficient for
  discrete gunshot sounds but wouldn't be for a continuous/looped world sound.

## Testing status

Compiled clean (`DX11-AVX|x64`, `xrEngine` target, 0 errors, only pre-existing unrelated warnings).
Not yet tested in-game as of this writing -- pending a weapon's `.ltx` authored with `fade_start`/
`fade_full` on its far/medium layers.
