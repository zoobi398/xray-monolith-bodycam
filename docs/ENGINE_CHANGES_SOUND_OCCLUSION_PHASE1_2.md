# Engine change: material-aware sound occlusion + diffraction (Phase 1 + 2, "snd_occlusion_mode 1")

Status: implemented and compiled (`DX11-AVX|x64`, `xrEngine` target, 0 errors). Three in-game test rounds
done (Garbage hangar); three real problems found and fixed -- see the "Update" sections below.

Builds on `04_ENGINE_CHANGES_SOUND_OCCLUSION_PHASE0.md` (pure measurement, already in place) and
implements Phases 1 and 2 of `OCCLUSION_ENGINE_PATCH.md` together, in a single pass (Option B: full
Fresnel/Maekawa diffraction from the start, not the simplified candidate-path version). `snd_occlusion_mode
0` (the default) is the exact, byte-identical original path -- nothing about it changed.

## Summary

- **Materials**: the original single flat `-8dB, no filter, no material awareness` occlusion is replaced,
  at `snd_occlusion_mode 1`, with a per-material-class system (`gamedata/configs/sound_occlusion.ltx`) --
  foliage/glass/wood/metal/masonry/terrain each have their own loss in dB, extra loss per metre of
  measured thickness, and a separate high-frequency loss that drives a real low-pass filter.
- **Multi-sample, deterministic**: instead of one ray to a random point (old system, re-rolled every
  frame -> audible flicker at obstacle edges), up to 5 fixed offset points around the source are traced
  and their results energy-averaged, giving smooth partial-occlusion instead of a coin-flip.
- **Diffraction (Phase 2, full physical model)**: when the direct path is significantly blocked, up to 5
  candidate over/around paths are tested; the shortest clear one's extra path length is converted to a
  frequency-dependent loss via the Maekawa diffraction approximation. The better of "through" and "around"
  always wins (`gain = max(direct, diffracted)`), never an average of blocked and clear.
- **Low-pass filter**: the engine's already-loaded-but-unused EFX filter objects are now actually used --
  both the direct signal and the reverb send get a live-updating low-pass, so an obstacle makes a sound
  duller, not just quieter.
- **Cost-controlled**: a per-frame evaluation budget and a reduced recalculation cadence for
  long/looped sounds (shots are always fully evaluated regardless of budget). Phase 0's baseline (Garbage,
  ~175s of real play) showed the *original* system already costs ~0.014ms/frame average -- there was
  enormous headroom, which is why this went straight to the full 5-sample + diffraction design rather
  than a cheaper 1-ray version.
- **Debug logging**: `snd_occlusion_debug 1` logs one line per evaluated NPC gunshot -- distance, gain in
  both ratio and dB, HF gain in both ratio and dB, blocked/clear, and whether diffraction won.

## Design choices worth flagging

1. **Why materials are resolved once per level, not per-hit.** `CDB::TRI`/`CDB::RESULT` already carry a
   14-bit material ID per triangle (confirmed by reading `xrCDB.h`/`xrCDB_ray.cpp`, not assumed) -- the
   exact same ID `GameMtlLib`'s `SGameMtl::GetID()` uses. `xrSound` cannot depend on `xrEngine`/`GMLib`
   (would be a backwards dependency), so `IGame_Level.cpp::BuildAndPushOcclusionMaterials()` (xrEngine,
   which already depends on GMLib) resolves every loaded material to an `SSoundOcclusionMaterial` once
   per level load and pushes a flat, ID-indexed table into `xrSound` via a new interface method. The hot
   path (`occ_trace_losses`) is then a single bounds-checked array lookup, no string comparisons.
2. **Why thickness is never guessed.** The original plan assumed matched entry+exit ray hits give a
   reliable thickness. STALKER's collision meshes are frequently single-sided (not watertight), so that
   pairing can silently fail. This implementation only ever applies `loss_db_per_m x thickness` when the
   very next hit (by distance) shares the same material -- a real measured pair. An unpaired hit still
   contributes its flat `loss_db` (a surface was crossed), just never a guessed thickness on top.
3. **Filters are attached once per voice, not re-bound every frame.** The filter *objects* are created
   once per pooled target (`_initialize()`) and bound to the source once at `render()` (voice start) if
   `snd_occlusion_mode 1` applies to that sound. Every frame after that, only the filter's own
   `AL_LOWPASS_GAINHF` *parameter* is updated (`fill_parameters()`) -- since the filter object is already
   attached, this takes effect live with no re-binding. Toggling the mode console command only affects
   sounds that (re)start after the toggle, same lifecycle as the pre-existing `Slot` (reverb send) binding
   this mirrors.
4. **Why the AL filter functions needed a passthrough.** `alGenFilters`/`alFilterf`/etc. are EFX extension
   function pointers, loaded once and stored as **private** members of `CSoundRender_CoreA` (verified by
   reading `SoundRender_CoreA.h`, not assumed from the original design doc, which had incorrectly assumed
   `m_is_supported`-style base-class access). `SoundRender_TargetA.cpp` doesn't have access to them
   directly, so `CSoundRender_Core` gained 3 thin virtual passthroughs
   (`occ_gen_filter`/`occ_delete_filter`/`occ_set_filter_lowpass`), no-ops in the base, overridden in
   `CSoundRender_CoreA` to call the real functions -- avoids a friend declaration or making the extension
   pointers public.
5. **Deviation from Option B's own doc where it mattered for realism.** The user's specific concern
   (silos/half-ruined walls at range -- partial cover that a handful of direct rays could easily
   mis-classify as "fully blocked") is precisely what diffraction with `gain = max(direct, diffracted)`
   solves: even if every direct sample hits solid geometry, the candidate search still finds the
   real gap/edge and lets it win. This is why diffraction (originally scoped as Phase 2, "later") shipped
   together with materials (Phase 1) in this pass instead of being deferred.

## What the original system does (Phase 0's own findings, unchanged, `snd_occlusion_mode 0`)

See `04_ENGINE_CHANGES_SOUND_OCCLUSION_PHASE0.md` for the full verified call chain. In short: one ray,
random point in a 0.2m sphere, re-rolled every frame, hit -> flat `-8dB` (`psSoundOcclusionScale`), no
material read, no filter.

## Files changed

### `src/xrSound/Sound.h`

New console-bound globals and the material struct + 2 new pure virtuals on `CSound_manager_interface`:

```cpp
XRSOUND_API extern int psSoundOcclusionMode;          // 0 = original path, 1 = this rework
XRSOUND_API extern float psSoundOcclusionStrength;    // global dB multiplier, 0-2
XRSOUND_API extern int psSoundOcclusionUpdateMs;       // recalc interval for long/looped sounds
XRSOUND_API extern int psSoundOcclusionBudget;         // max full evaluations per frame
XRSOUND_API extern int psSoundOcclusionDiffraction;    // 0/1
XRSOUND_API extern int psSoundOcclusionDebug;          // 0 off, 1 = per-shot log line

struct SSoundOcclusionMaterial
{
    float loss_db = 10.f;
    float loss_db_per_m = 1.f;
    float hf_loss_db = 12.f;
    bool ignore = false; // acoustically transparent (foliage, thin grass...)
};

// on CSound_manager_interface:
virtual void set_occlusion_materials(const SSoundOcclusionMaterial* table, u32 count) = 0;
virtual void set_occlusion_limits(float max_loss_db, float max_hf_loss_db, float max_thickness_m) = 0;
```

### `src/xrSound/SoundRender_Core.h`

`CSoundRender_Core` gains the material table, budget tracking, the result struct, and the new methods:

```cpp
xr_vector<SSoundOcclusionMaterial> m_occ_materials; // indexed directly by CDB material ID
SSoundOcclusionMaterial m_occ_default_material;
float m_occ_max_loss_db = 28.f, m_occ_max_hf_loss_db = 36.f, m_occ_max_thickness_m = 12.f;

u32 m_occ_budget_used = 0;
IC bool occ_budget_take() // true (and consumes budget) while under snd_occlusion_budget for this frame
{
    if (psSoundOcclusionBudget <= 0 || m_occ_budget_used < (u32)psSoundOcclusionBudget)
    { m_occ_budget_used++; return true; }
    return false;
}

struct SSoundOcclusionResult
{
    float gain = 1.f, gain_hf = 1.f;
    bool blocked = false, diffracted = false;
};

virtual void set_occlusion_materials(const SSoundOcclusionMaterial* table, u32 count) override;
virtual void set_occlusion_limits(float max_loss_db, float max_hf_loss_db, float max_thickness_m) override;
SSoundOcclusionResult get_occlusion_ex(const Fvector& src, u32 profile, LPCSTR debug_name = nullptr,
    bool is_weapon_shot = false); // profile: 0=light(1 sample) 1=impulse(5+diffraction) 2=loop(3)

private:
void occ_trace_losses(const Fvector& from, const Fvector& to, float& out_loss_db, float& out_hf_db, u32& rays);
SSoundOcclusionResult occ_try_diffraction(const Fvector& L, const Fvector& S, float direct_dist);

// Thin passthroughs to CSoundRender_CoreA's private AL filter function pointers (see design note 4).
virtual u32 occ_gen_filter() { return 0; }
virtual void occ_delete_filter(u32 id) {}
virtual void occ_set_filter_lowpass(u32 id, float gain, float gain_hf) {}
```

### `src/xrSound/SoundRender_Core_Processor.cpp` -- the actual algorithm

`occ_trace_losses`: one "all hits" ray (`ray_options(0)` -- neither `OPT_CULL` nor `OPT_ONLYFIRST` nor
`OPT_ONLYNEAREST`, confirmed by reading `xrCDB_ray.cpp::cform_ray_collider::_prim` that this combination
is what returns *every* crossed triangle rather than filtering to one), sorted by distance, walked once:

```cpp
void CSoundRender_Core::occ_trace_losses(const Fvector& from, const Fvector& to, float& out_loss_db,
    float& out_hf_db, u32& rays)
{
    // ... ray setup, geom_DB.ray_options(0); geom_DB.ray_query(geom_MODEL, from, dir, range); ...
    std::sort(hits.begin(), hits.end(), [](auto* a, auto* b) { return a->range < b->range; });
    for (size_t i = 0; i < hits.size(); ++i)
    {
        const SSoundOcclusionMaterial& mat = /* m_occ_materials[hits[i]->material], bounds-checked */;
        if (mat.ignore) continue;
        out_loss_db += mat.loss_db;
        out_hf_db += mat.hf_loss_db;
        if (i + 1 < hits.size() && hits[i + 1]->material == hits[i]->material)
        {
            const float thickness = _min(hits[i + 1]->range - hits[i]->range, m_occ_max_thickness_m);
            out_loss_db += mat.loss_db_per_m * thickness;
            ++i; // consumed as this pair's exit face
        }
    }
    out_loss_db = _min(out_loss_db, m_occ_max_loss_db);
    out_hf_db = _min(out_hf_db, m_occ_max_hf_loss_db);
}
```

`get_occlusion_ex`: deterministic offset pattern (`Fvector::generate_orthonormal_basis` around the
listener->source direction, +-0.35m along up/right, matching the "soft edges via averaging" plan), power-
domain energy averaging, then diffraction if still significantly blocked:

```cpp
CSoundRender_Core::SSoundOcclusionResult CSoundRender_Core::get_occlusion_ex(const Fvector& src,
    u32 profile, LPCSTR debug_name, bool is_weapon_shot)
{
    // n = 1/3/5 samples depending on profile; each traced via occ_trace_losses
    energy += powf(10.f, -loss_db / 10.f);  // power domain
    // ... averaged, then:
    res.gain = _sqrt(energy);                              // power avg -> amplitude gain
    res.gain_hf = powf(10.f, -(hf_db_sum / n) / 20.f);
    res.blocked = res.gain < 0.99f;

    if (res.gain < 0.5f && psSoundOcclusionDiffraction && profile != 0)
    {
        SSoundOcclusionResult diff = occ_try_diffraction(L, src, dist);
        if (diff.gain > res.gain) { res.gain = diff.gain; res.gain_hf = _max(res.gain_hf, diff.gain_hf);
            res.diffracted = true; res.blocked = false; }
    }
    if (psSoundOcclusionDebug >= 1 && is_weapon_shot) Msg("* [snd_occ] %s dist=... gain=... hf=... ...", ...);
    return res;
}
```

`occ_try_diffraction`: nearest blocker from each end (anchors the candidates near the actual obstruction,
not the geometric midpoint), 5 candidates (above the obstacle at +2/+6/+15m, +-8m to either side), each
tested with 2 cheap `OPT_ONLYFIRST` rays; the shortest clear detour's Maekawa attenuation
(`10*log10(3+20*N)`, Fresnel number `N = 2*delta/lambda`, computed separately at ~250Hz for gain and
~4kHz for the HF component) is the result:

```cpp
CSoundRender_Core::SSoundOcclusionResult CSoundRender_Core::occ_try_diffraction(const Fvector& L,
    const Fvector& S, float direct_dist)
{
    // H1 = nearest hit L->S, H2 = nearest hit S->L (OPT_ONLYNEAREST), mid = (H1+H2)/2
    // 5 candidates around mid; for each: 2x OPT_ONLYFIRST clear-path tests
    // path_len = dist(L,P) + dist(P,S); delta = max(path_len - direct_dist, 0)
    // N_gain = 2*delta/1.37f; N_hf = 2*delta/0.086f
    // db = min(10*log10f(3+20*N), 25.f); keep the candidate with the lowest db_gain
    // res.gain = powf(10, -best_db_gain/20); res.gain_hf = powf(10, -best_db_hf/20); res.diffracted = true;
}
```

### `src/xrSound/SoundRender_Emitter.h` / `SoundRender_Emitter_StartStop.cpp` / `SoundRender_Emitter_FSM.cpp`

New per-emitter state: `occluder_gain_hf`, `occ_target_gain`/`occ_target_hf` (the last full evaluation,
followed smoothly), `occ_next_update` (cadence gate), `occ_profile` (resolved once in `start()` from
`SOUND_TYPE_WEAPON_SHOOTING`/`SOUND_TYPE_WORLD_OBJECT_EXPLODING` -> impulse, else looped -> loop, else
light).

`update_culling()`'s 3D branch, mode-gated (mode 0 branch is the exact original code, untouched):

```cpp
if (owner_data->g_type == SOUND_TYPE_WORLD_AMBIENT) { occluder_volume = 1.f; occluder_gain_hf = 1.f; }
else if (psSoundOcclusionMode == 0) { /* exact original get_occlusion() + volume_lerp(..., 1.f, dt) */ }
else
{
    if (now >= occ_next_update && SoundRender->occ_budget_take())
    {
        auto r = SoundRender->get_occlusion_ex(p_source.position, occ_profile, ..., occ_profile == 1);
        occ_target_gain = r.gain; occ_target_hf = r.gain_hf;
        occ_next_update = now + psSoundOcclusionUpdateMs / 1000.f;
    }
    volume_lerp(occluder_volume, occ_target_gain, 4.f, dt);   // faster than mode 0's fixed 1.0/s
    volume_lerp(occluder_gain_hf, occ_target_hf, 4.f, dt);
}
```

The `stStarting`/`stStartingLooped` cases (a shot's own defining sound) bypass the budget and smoothing
entirely at mode 1 -- always fully evaluated, assigned directly, same intent as mode 0's existing
"assign directly, let update_culling's tiny first-frame lerp step take over from there" pattern.

### `src/xrSound/SoundRender_TargetA.h/.cpp` -- the low-pass filter

Filters created once per pooled target (`_initialize()`, if EFX is supported, via the new passthroughs),
bound to the source once at voice start (`render()`) only if mode 1 applies to that sound, then their
*parameters* updated live every frame (`fill_parameters()`, cache-guarded like `AL_GAIN`/`AL_PITCH`
already are). The reverb-send filter gets a milder `sqrt(hf)` cut -- reflected/reverberant energy reaches
a listener around obstacles more easily than the direct path.

### `src/xrSound/SoundRender_CoreA.h/.cpp`

3 new public overrides (`occ_gen_filter`/`occ_delete_filter`/`occ_set_filter_lowpass`) calling the
private `alGenFilters`/`alFilteri`/`alFilterf`/`alIsFilter`/`alDeleteFilters` function pointers.

### `src/xrEngine/IGame_Level.cpp`

New `BuildAndPushOcclusionMaterials()`, called once per level load right after the existing
`Sound->set_geometry_occ(...)`. Loads `gamedata/configs/sound_occlusion.ltx` as a standalone `CInifile`
(same pattern `GameMtlLib.cpp` already uses for `materials/materials.ltx` -- not merged into
`pSettings`/`system.ltx`, which this repo doesn't ship a copy of). For every loaded `GameMtl`: first
substring match in `[sound_occlusion_classes]` wins; else its own legacy `sound_occlusion_factor`
(`materials/*.ltx`) converted to a `loss_db` if set; else `[default]`.

### `src/xrEngine/xr_ioc_cmd.cpp` / `Stats.cpp` (Phase 0, already had the `OCC:`/`OCC AI:` lines and
`snd_occlusion_stats`)

6 new console commands, same `CMD4` idiom as every other sound cvar in this file:

```cpp
CMD4(CCC_Integer, "snd_occlusion_mode", &psSoundOcclusionMode, 0, 1);
CMD4(CCC_Float, "snd_occlusion_strength", &psSoundOcclusionStrength, 0.f, 2.f);
CMD4(CCC_Integer, "snd_occlusion_update_ms", &psSoundOcclusionUpdateMs, 50, 1000);
CMD4(CCC_Integer, "snd_occlusion_budget", &psSoundOcclusionBudget, 1, 64);
CMD4(CCC_Integer, "snd_occlusion_diffraction", &psSoundOcclusionDiffraction, 0, 1);
CMD4(CCC_Integer, "snd_occlusion_debug", &psSoundOcclusionDebug, 0, 2);
```

### `gamedata/configs/sound_occlusion.ltx` (new)

Material classes (substring-matched against GameMtl names), their `loss_db`/`loss_db_per_m`/
`hf_loss_db`/`ignore`, and the `[sound_occlusion_limits]` caps. Values are explicitly documented as
starting points to tune by ear, not physical measurements (a real concrete wall is 40+dB, which would
make firefights through walls inaudible).

### `gamedata/scripts/options_modded_exes_sound_occlusion.script` (new) + `options_modded_exes_gameplay.script`
+ `st_bodycam_mcm.xml`

A third "Modded Exes" sub-page (alongside the existing Bodycam Weapon Recoil/Sway ones), using the stock
`track`/`list_bool` widgets bound directly to a console command by name (`cmd = "snd_occlusion_mode"` etc.
-- the same idiom the base game's own `options_modded_exes_hdr10.script` uses for its `r4_hdr10_*`
commands, verified by reading that file rather than assumed), not the Bodycam Lua bridge the recoil/sway
pages use (these are plain xrEngine console commands, not Bodycam settings).

**Gotcha reproduced here on purpose**: writing the page script and its `st_bodycam_mcm.xml` labels is not
enough on its own -- the page must also be imported and added to `options_modded_exes_gameplay.script`'s
`GROUP` table (`local page_sound_occlusion = options_modded_exes_sound_occlusion.PAGE`, then listed inside
`GROUP = group { ... }`). This project's own first pass shipped without that step: the page compiled fine,
the labels were correct, and it was simply invisible in the in-game menu until caught and fixed. The
`options_modded_exes_sound_duck.script` page added later (`06_ENGINE_CHANGES_ACTOR_FIRE_DUCKING.md`) got
this right the first time specifically because of this entry -- do the same for any new Modded Exes page.

## Backward compatibility

`snd_occlusion_mode` defaults to `0`. At mode 0: `update_culling`'s occlusion branch runs the exact
original 4 lines (`get_occlusion` + `volume_lerp(..., 1.f, dt)`), the `stStarting`/`stStartingLooped`
cases run the exact original single line, and `SoundRender_TargetA`'s filters are created (harmless,
unused objects) but never bound to a source (`AL_DIRECT_FILTER`/`AL_AUXILIARY_SEND_FILTER` stay
`AL_FILTER_NULL`, exactly as before this change existed). No existing behaviour changes unless a player
or the shipped `.ltx`/console config explicitly turns mode 1 on.

## Points of attention

1. **Never total silence.** `max_loss_db`/`max_hf_loss_db` (`sound_occlusion.ltx`) cap the direct path,
   and diffraction's `gain = max(direct, diffracted)` means a real nearby gap or edge will surface even
   if every direct sample is blocked -- addresses the "false negative on complex partial cover" concern
   this design was specifically revised for (see design note 5).
2. **Mode toggles apply to new sounds, not retroactively** to already-playing ones (matches the existing
   `Slot`/reverb-send lifecycle this mirrors) -- expected for a debug/comparison toggle.
3. **`sound_occlusion.ltx` is read once per level load.** Edits need a level reload (or restart) to take
   effect -- not a live-reloadable file.
4. **AI hearing is untouched.** `get_occlusion_to`/`SoundEvent_Register` (Phase 0's own findings) still
   use only the `.som` mesh, regardless of `snd_occlusion_mode`. `OCCLUSION_ENGINE_PATCH.md`'s optional
   Phase 3bis (`snd_occlusion_ai`) was not implemented -- deliberately out of scope, changes AI detection
   balance.
5. **Diffraction candidate positions are axis-aligned to the listener->source direction**, not to level
   geometry -- for very irregular obstacles (a genuinely maze-like ruin) the 5 fixed candidates may all
   miss a real gap that a differently-placed 6th point would have found. Increasing sample density is a
   possible follow-up if this proves insufficient in testing.
6. **Near-fade (`03_ENGINE_CHANGES_NEAR_FADE.md`) is unaffected and composes cleanly** -- it decides
   *whether a layer plays at all* at shot time in `HudSound.cpp`, before this system ever runs.

## Test plan

1. `snd_occlusion_mode 0`: confirm identical feel/numbers to the Phase 0 baseline (same 5 scenes).
2. `snd_occlusion_mode 1`, `snd_occlusion_debug 1`:
   - foliage between listener and an NPC -> near-zero loss logged;
   - a real concrete wall -> substantial loss + heavy HF cut;
   - a corner -> stable result shot to shot (no more flicker);
   - the user's specific silo/ruined-wall scenario -> `diffracted=true` in the log when the direct path
     alone would read fully blocked.
3. Garbage hills: NPC behind a rise should stay audible but duller, not silent.
4. Non-regression: mode 0 numbers match the Phase 0 baseline; player's own (HUD) shots unaffected (2D,
   never occluded); AI hearing unaffected.
5. Performance: `snd_occlusion_stats 1` in both modes, same 5 scenes, compare `[snd_occ]` log lines --
   given Phase 0's baseline (~0.014ms/frame, ~634 rays/s average at Garbage), mode 1's 5x-sample-plus-
   diffraction cost is expected to stay well under the plan's own <0.2ms/frame target even in the
   heavier scenes not yet measured (firefight, Jupiter corridors, anomaly-dense area).

## Update (30/09) -- reverb send was coupled to the direct path's occlusion gain

First in-game test (Garbage hangar, indoor NPC vs. indoor player) found a real problem: ducking behind
cover mid-shot dropped the volume by ~20dB almost instantly, feeling like a mute switch rather than "the
sound gets duller". `snd_occlusion_debug 1`'s log confirmed it precisely: of ~16k evaluated shots, 11103
were `BLOCKED` -- and *every single one* sat at exactly `gain=0.04 (28.0dB)`, the `max_loss_db` ceiling --
vs. only 733 where diffraction found a usable detour (13-24dB loss, still audible). Diffraction was
failing far more often than succeeding (a real but secondary issue, see "Known limitations" below), but
that alone doesn't explain the *mute-switch* character.

**Root cause**: `occluder_volume` (the occlusion gain) fed the emitter's single shared `AL_GAIN`, which
OpenAL applies to *both* the direct signal and the reverb auxiliary send. In an indoor space, SAR
(`469- Spatial Audio Rework`, inspected directly -- its `sar_main.script` only ever rewrites the shared
EAX reverb *effect's* own properties via `snd_efx_reverb_overwrite_*`, once globally; it never touches a
per-voice `AL_GAIN` or filter) already computes a real room reverb meant to carry a shot's energy around
the space via reflections. But since occlusion's gain hit `AL_GAIN` upstream of everything, ducking behind
cover throttled the reverb send by the same ~28dB as the direct sound -- killing the one channel that
should have kept the shot audible-but-duller. No conflict with SAR itself (confirmed by reading its
script: it never touches per-voice state), just an architecture bug on this side.

### Fix: decouple the direct and reverb-send gains

`AL_GAIN` (mode 1, 3D emitters) no longer carries occlusion at all -- it goes back to being driven only
by distance/fade, exactly as if occlusion didn't exist. The occlusion gain instead lives entirely in the
two filters' own broadband `AL_LOWPASS_GAIN`, independently:

- `m_direct_filter`'s gain = `occluder_volume` (the direct-path result, unchanged meaning, just a new
  consumer).
- `m_send_filter`'s gain = a new, separately smoothed `occluder_gain_wet`, computed once per evaluation
  in `get_occlusion_ex()`:
  ```cpp
  const float wet_sensitivity = clamp(psSoundOcclusionWetSensitivity, 0.f, 1.f); // new cvar, default 0.35
  res.wet_gain = 1.f - wet_sensitivity * (1.f - res.gain);
  ```
  `wet_sensitivity = 0` -> the reverb send is never touched by occlusion at all (SAR's own room reverb is
  the only thing shaping it); `1` -> the old fully-coupled behaviour. Exposed as
  `snd_occlusion_wet_sensitivity` (console + the new MCM page), default `0.35` -- a real obstacle still
  measurably dampens the reflected field (a closed door does reduce reverb loudness in reality, just far
  less than it kills the direct line), but nowhere near as hard as the direct signal.

New/changed state: `SSoundOcclusionResult::wet_gain`; emitter fields `occluder_gain_wet`,
`occ_target_wet_gain`; `SoundRender_TargetA` gained `cache_gain_direct`/`cache_gain_wet` (the existing
`cache_hf` cache-guard pattern, now covering three independently-varying filter parameters instead of
one). `occluder_volume` itself is **repurposed** at mode 1 only: it still means "direct-path occlusion
gain", it just no longer reaches `AL_GAIN` -- `smooth_volume`'s formula uses a local `occ_for_gain` that's
forced to `1.f` for 3D emitters at mode 1, `occluder_volume` unchanged at mode 0 (still byte-identical to
the original there). The debug log line gained a `wet=` field alongside `gain=`/`hf=`.

## Update (30/09) -- diffraction missed nearby openings (near-anchor candidates)

Second round of user feedback, with a concrete case: standing just inside an open train wagon door in an
otherwise wide-open hangar (see the sketch and screenshots the user provided), ducking a step or two to
put the wagon's thin wall between self and an NPC produced a much bigger volume drop than the mostly-clear
line of sight (through the doorway, into the open hangar) should have caused.

**Root cause**: confirmed by re-reading `occ_trace_losses` and `occ_try_diffraction` directly rather than
re-deriving from memory. Two compounding facts:
1. The direct-path loss (`occ_trace_losses`) depends purely on the material/thickness crossed by the
   straight line between listener and source sample point -- distance to the source has no effect on it
   at all. `get_occlusion_ex`'s 5 direct-path samples are jittered only +-0.35m around the source, far too
   small to ever land in a doorway a metre or two wide, so the direct path always reads "100% wall" the
   instant the straight line crosses one.
2. Diffraction (meant to rescue exactly this case) placed its 5 candidates at **fixed absolute offsets**
   from the *midpoint* of the listener-source segment (+2/+6/+15m vertical, +-8m lateral) -- scaled for
   "go over/around a large obstacle seen from a distance" (a wall or silo tens of metres away), not "step
   through a doorway one metre from where I'm standing". For the wagon-door case none of the 5 candidates
   ever lands anywhere near the real opening, so diffraction fails and the result sits at the hard
   `max_loss_db` ceiling -- this is the same ~6% diffraction-success-rate limitation flagged below, now
   with a concrete, reproducible cause instead of just a log statistic.

Direct answer to the user's question ("est-ce que c'est la distance ou l'epaisseur de mur qui compte ?"):
in this system it is purely the material/thickness the straight line crosses -- the fix below is what
makes physical closeness to an opening matter, via diffraction's own path-length-delta term (Fresnel/
Maekawa), not the direct path.

### Fix: near-anchor candidates in addition to the wide ones

`occ_try_diffraction` (`SoundRender_Core_Processor.cpp`) keeps its original 5 "wide" candidates (still the
right shape for a large obstacle at range) and adds a second set anchored at the two real blocker hit
points (`H1`, the nearest surface from the listener's side; `H2`, from the source's side) instead of the
segment's midpoint, at two much smaller scales (0.6m and 2m), sideways and up:

```cpp
const float near_scales[2] = { 0.6f, 2.f };
const Fvector* near_anchors[2] = { &H1, &H2 };
for (u32 a = 0; a < 2; ++a)
    for (u32 sc = 0; sc < 2; ++sc)
    {
        const float o = near_scales[sc];
        candidates[cand_count++].mad(*near_anchors[a], up, o);
        candidates[cand_count++].mad(*near_anchors[a], right, o);
        candidates[cand_count++].mad(*near_anchors[a], right, -o);
    }
```

17 candidates total (5 wide + 12 near) instead of 5, each still validated by two real `OPT_ONLYFIRST` ray
casts before being accepted -- no geometry is guessed, every accepted candidate is a genuinely clear
detour. "Down" is skipped for the near set (there's rarely anything to duck under indoors). Cost: roughly
3x the rays per diffraction attempt, but diffraction only ever runs when the direct path is already >50%
blocked, and only for weapon-shot/looped profiles under the existing budget/cadence gate -- negligible
against Phase 0's measured headroom (~0.014ms/frame baseline for the *original*, much cheaper system).

No geometry export/SDK step was needed -- the system already ray-casts the real, live collision mesh per
evaluation; the gap was in where candidates were placed, not in what data was available.

## Update (01/10) -- one-shot voices were being re-occluded live, after the sound had already left the source

Third round of user feedback: ducking behind a container while an NPC gunshot's tail was still playing
caused a near-instant volume drop mid-playback -- unrealistic, since sound that has already left the
source doesn't retroactively re-route around an obstacle the listener ducks behind afterward, and a
one-shot's whole lifetime (a crack + short tail) is far too short for the listener's real position to
have moved meaningfully anyway.

**Root cause**: `update_culling()`'s mode-1 branch re-evaluates occlusion on its own cadence
(`snd_occlusion_update_ms`) and smoothly follows the result via `volume_lerp` for the ENTIRE lifetime of
every voice, not just the moment it starts -- correct for a genuinely sustained sound (the listener can
walk behind real cover mid-loop), wrong for a one-shot (nothing should change after `stStarting`'s single
evaluation, which already captured the correct geometry at the instant the sound was emitted).

The obvious-looking fix (freeze for `occ_profile == 1` "impulse") would have been wrong: the user pointed
out that their own `TRUE_CYCLIC_SOUNDS_V1_VERDATIM` mod plays a genuinely *looped* sample during a
sustained actor burst, and clarified two things that resolved the apparent conflict: (1) that system only
ever applies to the actor's own weapon, never to NPCs (who always use the stock `snd_shoot`/`snd_N_layer`
collections -- discrete one-shot samples repeated at the weapon's RPM interval, never a continuous sample
start/stopped in sync with the trigger), and (2) the actor's own weapon sound never passes through
occlusion at all regardless (`b_hud_mode`/`b2D`, "exterior sources only" -- confirmed by re-reading
`update_culling`'s `b2D` branch, which never touches occlusion). So `occ_profile` was never actually the
right signal either way -- a sustained weapon-fire loop is still classified `occ_profile == 1` "impulse"
by `g_type`, independent of whether that particular voice loops.

### Fix: gate re-evaluation on the voice's own loop state, not its profile

New per-emitter field `occ_is_loop` (`SoundRender_Emitter.h`), resolved once in `start()` from the actual
`_loop` parameter (`SoundRender_Emitter_StartStop.cpp`) -- independent of `occ_profile`. In
`update_culling()` (`SoundRender_Emitter_FSM.cpp`), the mode-1 occlusion branch became
`else if (occ_is_loop) { ...unchanged re-evaluation/smoothing... }` with no trailing `else`: a one-shot
voice (`occ_is_loop == false`) simply falls through and does nothing, leaving `occluder_volume`/
`occluder_gain_hf`/`occluder_gain_wet` exactly as `stStarting`/`stStartingLooped` set them at emission.
A looped voice (sustained NPC full-auto using the stock layered system would still be one-shot per
repeated crack -- this only matters for something that is *itself* a single long-duration looped sample,
e.g. the actor's own `TRUE_CYCLIC_SOUNDS` fire loop if it were ever subject to occlusion, or any future
looped world/NPC sound) keeps the original continuous tracking, unchanged.

Backward compatible the same way every other piece of this rework is: `snd_occlusion_mode 0` is untouched
(this only lives inside the mode-1 branch), and mode 1 behaves identically to before for every looped
voice -- only one-shot voices (the overwhelming majority of what occlusion actually processes: every NPC
gunshot, footstep, impact) change, from "re-evaluated live for their whole playback" to "evaluated once,
correctly, at the moment they were emitted".

## Update (02/10) -- the one-shot snapshot was taken at voice start, not at shot time

The 01/10 freeze-at-emission fix didn't fully cure "I duck behind cover while the NPC shot's tail is still
playing and the tail gets occluded". Cause: the snapshot was taken at `stStarting`, i.e. when the *voice*
starts -- and many NPC gunshot voices start later than the shot itself: layers with a `.ltx` delay
(typically the tails) and the distance-based propagation delay in `update()` (>50m, up to 3.5s). Those
sit in `stStartingDelayed` meanwhile, so hiding during the wait baked "behind cover" into the voice.

Fix: new `occ_snapshot_valid` (`SoundRender_Emitter.h`, reset in ctor/`start()`). `update()` now takes the
occlusion snapshot on the first update with a valid, non-origin position for any non-looped, non-ambient
3D voice in a pre-playing state (including the delayed ones) and stores it in `occ_target_*`;
`stStarting` reuses it, evaluating itself only as a fallback. Looped voices unchanged. Cost is unchanged
(still one `get_occlusion_ex` per one-shot voice, just earlier). Deliberate choice: this also snapshots
across the propagation delay, which is arguably the *less* physical option (a wave that arrives after you
hid really is occluded) -- chosen because the stated goal is that nothing the player does after the shot
should re-shape that shot's sound.

## Update (02/10) -- indoor occlusion values (`snd_occlusion_indoor_mode`)

Motivation (from a real session log, 2270 voices): at `strength` 0.3 the effective cap was 12 x 0.3 =
3.6dB (strength is applied AFTER the cap), 24% of voices sat on it, almost all at 50-200m, and diffraction
(only tried above 6dB) never ran. Analytically, with `loss_db` + `loss_db_per_m` and a 12dB cap a single
masonry wall saturates at 3.5m thickness (metal 4m, wood 9m), so 5m and 15m of wall are indistinguishable;
`hf_loss_db` has no per-metre term at all (flat per surface, 36dB cap fills in ~3 surfaces). One set of
limits cannot be right for both forest clutter (many thin hits) and thick-walled corridors.

What changed:
- Only the limits differ, not the material table: `strength`, `max_loss_db`, `max_hf_loss_db` are each
  interpolated between their outdoor and indoor value by `snd_occlusion_indoor_factor` (0..1).
  **Rule of where things live (settled the same day, after a first version that mixed them):**
  `sound_occlusion.ltx` = acoustic data (materials + caps, outdoor AND indoor: `max_loss_db_indoor`,
  `max_hf_loss_db_indoor` in `[sound_occlusion_limits]`, default to the outdoor caps when absent, read once
  per level load); menu/console = behaviour (`snd_occlusion_strength`, `snd_occlusion_indoor_strength`
  default 1.0, `snd_occlusion_indoor_mode` 0 = factor ignored / byte-identical to before, plus the
  existing cadence/budget/diffraction/wet cvars; `snd_occlusion_indoor_factor` is script-driven).
  `set_occlusion_limits` gained the two indoor caps. MCM page gained the toggle and the indoor strength.
- `gamedata/scripts/sound_occlusion_indoor.script` pushes the factor: polls `sar_main.current_score` every
  250ms, maps it linearly between SAR's own lower/upper thresholds (0.4/0.75 fallback), and sends the
  console command only when it moved by >= 0.02 (endpoints always sent). SAR missing => factor stays 0.
  Verified in `sar_main.script`: SAR measures every 750ms and eases `current_score` toward its target, so
  250ms polling only samples an already-smooth value.
- Which score: the ACTOR's. SAR's per-NPC `isIndoor` (sar_snd_replacer.script) casts its rays from
  `getCharPos()` which defaults to the actor, with directions rotated by the NPC's heading -- noisy
  per-NPC and not about the NPC's surroundings. Source-indoor/listener-outdoor is therefore the weak
  combination; the `masonry` class (building walls) is what covers it.
- Debug log lines gained `raw=` (mean pre-cap, pre-strength material sum -- how far past the cap the
  geometry really goes) and `ind=` (indoor factor in effect). Use these to check how often each scene
  saturates.

Suggested test: `snd_occlusion_strength` back to 1.0, `snd_occlusion_indoor_mode 1`, compare `raw=` and
cap-hit rate in Red Forest vs Jupiter corridors.

## Known limitations / possible follow-ups

- **Diffraction candidate placement is still a fixed, hand-picked scheme** (5 wide + 12 near, see update
  above), not a true edge/aperture search. It should now catch both "large obstacle at range" and "narrow
  opening a metre or two away", but an opening at an intermediate distance (say 5-10m) or one that doesn't
  align with the up/right axes relative to the listener-source line could still be missed. Re-test in the
  wagon/hangar scenario and the original silo/ruined-wall case; widen further (more scales, or a proper
  aperture search) only if gaps are still found in practice.
- `max_loss_db`/`max_hf_loss_db` (28dB/36dB) are still the hard ceiling on the *direct* path when
  diffraction fails entirely -- worth revisiting once both fixes have been felt in-game; may not need
  lowering at all now that the reverb send no longer collapses alongside it and diffraction catches more
  cases.
- **Diffraction's own loss has a separate, hard-coded 25dB ceiling** (`occ_try_diffraction`'s
  `_min(10.f*log10f(3.f+20.f*N), 25.f)`), entirely independent of `sound_occlusion.ltx`'s `max_loss_db`.
  Neither `max_loss_db` nor `snd_occlusion_strength` (which only scales the *direct* path's `loss_db`,
  never diffraction's Maekawa result) can soften a case where diffraction succeeds but its own detour
  still costs 15-20dB+. Not exposed as a tunable at all yet -- would need its own cvar if a case like this
  shows up in testing (so far: tuning guidance below assumes the direct path is the thing being adjusted).

### Tuning guide (01/10) -- which lever for which symptom

Worked out in chat while tuning the first live session; capturing it here since it isn't obvious from the
code alone:

- **"Occlusion feels too strong everywhere, uniformly"** -> `snd_occlusion_strength` (live, MCM). It
  scales every material's `loss_db`/`hf_loss_db` by the same factor in one shot -- mathematically
  equivalent to editing all of `sound_occlusion.ltx`'s classes at once, without the tedium, and without a
  level reload.
- **"One specific material (e.g. metal) feels disproportionate relative to the others"** -> edit that
  material's own section in `sound_occlusion.ltx` (file edit + level reload). `strength` can't target a
  single class; this is the only lever that can.
- **"Only the worst cases (many stacked obstacles, or diffraction failing) feel excessive, everyday
  single-obstacle cases feel fine"** -> `max_loss_db`/`max_hf_loss_db` in `sound_occlusion.ltx`. Doesn't
  touch anything that isn't already near the ceiling, so it's a scalpel for extremes, not a general
  "turn it down" knob.
- **A single obstacle in an otherwise wide-open area (crate/box, not a real wall) feels too occluded**
  (the original wagon/hangar complaint) -> `snd_occlusion_wet_sensitivity` first (live, MCM -- lets the
  room's own reflected energy carry the sound even when the direct line is blocked; most effective
  indoors where there's real reverb to draw on, much less so outdoors). `max_loss_db` second if
  diffraction still isn't finding a clean detour around that specific obstacle.

## Testing status

Compiled clean (`DX11-AVX|x64`, `xrEngine` target via `engine-vs2022.sln`, 0 errors, only pre-existing
unrelated warnings). Three in-game rounds done (Garbage hangar): first found and fixed the wet/dry
coupling bug, second found and fixed the diffraction candidate-placement gap, third found and fixed
one-shot voices being re-occluded live after emission (all above, all compiled and deployed). Not yet
re-tested after the freeze-at-emission fix, and the other 4 baseline scenes (firefight, Garbage hills,
Jupiter corridors, anomaly-dense area) still remain to be tried with mode 1.
