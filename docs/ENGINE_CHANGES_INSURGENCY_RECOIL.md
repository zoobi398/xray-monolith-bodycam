# Engine changes: Insurgency-style recoil, shot-progression animation tiers, recoil decompensation

Status: implemented and compiled (`DX11-AVX|x64`), tested in-game on the UZI and Kriss Vector.

Base: X-Ray Monolith Bodycam MT (same base as `01_ENGINE_CHANGES_FADE_SYSTEM.md`), working copy
`xray-monolith-bodycam_insurgency_recoil-2026.9.12-mt`.

## Summary

Three related but independently-toggleable, per-weapon opt-in systems, all defaulting to exactly the
original stock behaviour for any weapon that doesn't set the new `.ltx` keys:

1. **Insurgency-style camera recoil** (`insurgency_recoil` and friends) -- replaces the stock
   `CWeaponShotEffector::Shot2` horizontal-kick formula with one that has short-term memory (AR(1)) and
   optional lean coupling, adds an optional rise-time ease-in on the vertical kick, an optional
   muzzle-pivot viewmodel rotation, and a continuous horizontal center-pull.
2. **Shot-progression animation tiers** (`insurgency_shot_anim_sustain`) -- makes full-auto shot
   animations look progressively less "snappy" across a sustained burst by swapping which pre-authored
   animation clip plays per shot, instead of always hard-cutting back to the same frame-0 clip. Includes
   a Blender addon that generates the tiered clips from a single source animation.
3. **Recoil decompensation** (`insurgency_decomp_*`) -- a one-shot cosmetic viewmodel kick (dip + forward
   push + anti-rise rotation) that plays once, right as a sustained burst genuinely ends, simulating the
   shooter's own compensating force suddenly having nothing left to fight. Gated on a minimum shot count
   and fully tunable per weapon, on top of 6 global MCM sliders.

All three only ever touch the **viewmodel** (Bodycam's cosmetic layer) and, for recoil itself, the
existing `CWeaponShotEffector`/`CCameraShotEffector` machinery that already drove the real camera --
nothing new was added that bypasses or duplicates that pipeline.

## Motivation

The stock X-Ray `CameraRecoil`/`CWeaponShotEffector` system (2008-era) has two properties that don't
match real full-auto shooting footage (reference: Insurgency: Sandstorm, used as the source for
reverse-engineering the target feel):

- The horizontal ("sideways") kick is a fresh independent random draw every shot, scaled by the
  *accumulated* vertical angle -- so it starts small and grows 3-4x over a sustained burst. Real footage
  shows the sideways step's magnitude roughly constant through a burst, with a weak but real tendency to
  keep drifting the same direction for a few shots in a row (not memoryless).
- The shot animation hard-cuts (no blending, verified in `SkeletonAnimated.cpp`/`player_hud.cpp` --
  `PlayHUDMotion(..., bMixIn2=false)` always takes the `LL_CloseCycle` + instant `blendAmount=1` path,
  never `LL_FadeCycle`) back to the same clip, frame 0, every single shot. At full-auto cadences faster
  than the clip's own length (common: 600rpm = 100ms between shots vs. ~1s clips) only the opening ~3
  frames are ever seen, repeated, and the clip's own settle/decompensation tail never plays.
- There is no concept at all of "the shooter's compensation suddenly releasing" when a burst ends --
  real full-auto footage shows a visible dip/release kick right after the last shot, which is entirely
  absent from stock recoil (which just relaxes linearly back to 0).

---

# Part A -- Insurgency-style camera recoil (AR(1) yaw memory + lean coupling)

## Files changed

### `src/xrGame/CameraRecoil.h`

`CameraRecoil` (the per-weapon recoil parameter struct, one instance for hip `cam_recoil` and one for
ADS `zoom_cam_recoil` on every `CWeapon`) gains 6 new fields, all defaulting to values that reproduce
stock behaviour exactly:

```cpp
bool InsurgencyRecoil;   // opt-in switch; false = CWeaponShotEffector::Shot2 stock path, untouched
float YawRho;            // AR(1) coefficient on the horizontal step; 0 = stock memoryless step
float LeanCoupling;      // gain on the actor's current roll when rotating the shot impulse; 0 = no lean bias
float RiseTimeMs;        // ms for the camera to ease into a fresh kick; 0 = instant (stock shape)
float MuzzlePivot;       // 0-1+: how much of Bodycam's vertical viewmodel rotation redirects around a
                         // muzzle-heavy pivot instead of rotating the whole viewmodel as one rigid
                         // block; 0 = current rigid behaviour, unchanged. See bodycam_simulation.cpp.
float YawCenterPull;     // 1/s: continuous exponential pull of the horizontal recoil angle back toward
                         // 0, independent of cam_return/Relax() (which stays exactly as configured,
                         // vertical included). 0 = no pull.
```

Defaults: `InsurgencyRecoil(false)`, `YawRho(0.0f)`, `LeanCoupling(0.0f)`, `RiseTimeMs(0.0f)`,
`MuzzlePivot(0.0f)`, `YawCenterPull(0.0f)`. All 6 added to `Clone()` (used to copy hip -> ADS defaults).

*(This struct also carries the Decomp* fields for Part C -- see below.)*

### `src/xrGame/Weapon.cpp` -- `CWeapon::Load()`

Read right after the existing `cam_dispersion_frac` load:

```cpp
cam_recoil.InsurgencyRecoil = !!READ_IF_EXISTS(pSettings, r_bool, section, "insurgency_recoil", FALSE);
cam_recoil.YawRho = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_yaw_rho", 0.0f);
cam_recoil.LeanCoupling = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_lean_coupling", 0.0f);
cam_recoil.RiseTimeMs = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_rise_time_ms", 0.0f);
cam_recoil.MuzzlePivot = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_muzzle_pivot", 0.0f);
cam_recoil.YawCenterPull = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_yaw_center_pull", 0.0f);
m_altAimLeanCoupling = !!READ_IF_EXISTS(pSettings, r_bool, section, "insurgency_alt_aim_lean", FALSE);
```

Cloned to `zoom_cam_recoil` right after (hip -> ADS default), then optionally overridden per-field if
`zoom_insurgency_yaw_rho` / `zoom_insurgency_lean_coupling` / `zoom_insurgency_rise_time_ms` /
`zoom_insurgency_muzzle_pivot` / `zoom_insurgency_yaw_center_pull` exist in the `.ltx` (same
`pSettings->line_exist(section, "...")` idiom used everywhere else in this function for the
`zoom_cam_*` overrides):

```cpp
if (pSettings->line_exist(section, "zoom_insurgency_yaw_rho"))
    zoom_cam_recoil.YawRho = pSettings->r_float(section, "zoom_insurgency_yaw_rho");
// ...same pattern for lean_coupling / rise_time_ms / muzzle_pivot / yaw_center_pull
```

`m_altAimLeanCoupling` is a `CWeapon` member (`bool`, declared alongside the other member fields), used
only in `on_weapon_shot_start()` below -- lets a weapon force a fixed alt-aim tilt angle in place of the
actor's real camera roll when a specific alternate-ADS mode is active (see next section).

### `src/xrGame/EffectorShot.h` -- `CWeaponShotEffector`

New protected state:

```cpp
// Insurgency-style recoil: AR(1) state for the horizontal step (CameraRecoil::YawRho).
// Unused (stays 0) unless CameraRecoil::InsurgencyRecoil is set.
float m_last_horz_step;

// Magnitude of the vertical delta this shot actually applied (post lean-rotation), so callers
// (Bodycam's cosmetic viewmodel kick) can scale their own impulse to the real recoil instead of
// using a fixed constant. Set every shot regardless of InsurgencyRecoil.
float m_last_shot_impulse;

// Insurgency-style recoil: camera-facing output, eased toward m_angle_vert/horz over
// CameraRecoil::RiseTimeMs on the way up (a fresh kick), tracked instantly on the way down (the
// already-validated linear Relax() shape, untouched). Equals m_angle_vert/horz exactly whenever
// RiseTimeMs is 0 or InsurgencyRecoil is off.
float m_output_vert;
float m_output_horz;
```

`Shot()`/`Shot2()` both gain an optional `float actor_roll = 0.f` parameter (default preserves every
existing call site untouched, e.g. `WeaponStatMgunFire.cpp`'s `Shot2(0.01f)` stays inert since its
`CameraRecoil` is default-constructed, i.e. `InsurgencyRecoil == false`).

New public getters (all trivial `IC` forwarders onto `m_cam_recoil`/internal state):

```cpp
IC float GetLastShotImpulse() { return m_last_shot_impulse; }
IC float GetOutputVert() const { return m_output_vert; }
IC float GetOutputHorz() const { return m_output_horz; }
IC bool IsInsurgencyRecoil() const { return m_cam_recoil.InsurgencyRecoil; }
IC float GetYawCenterPull() const { return m_cam_recoil.YawCenterPull; }
IC float GetMuzzlePivot() const { return m_cam_recoil.MuzzlePivot; }
```

### `src/xrGame/EffectorShot.cpp` -- `CWeaponShotEffector::Shot2()`

The core of the whole system. `dvert` (vertical kick) is computed exactly as before (untouched). What
changes is everything after, branched on `m_cam_recoil.InsurgencyRecoil`:

```cpp
void CWeaponShotEffector::Shot2(float angle, float actor_roll)
{
    float dvert = angle * (m_cam_recoil.DispersionFrac + m_Random.randF(-1.0f, 1.0f) * (1.0f - m_cam_recoil.DispersionFrac));

    if (m_cam_recoil.InsurgencyRecoil)
    {
        // AR(1) horizontal step with a CONSTANT amplitude envelope (cam_step_angle_horz), deliberately
        // NOT scaled by the accumulated vertical angle (unlike stock). YawRho == 0 reduces this to a
        // fresh random draw each shot, still at constant amplitude (not the stock coupled shape).
        float rdm = m_Random.randF(-1.0f, 1.0f);
        float base_step = m_cam_recoil.StepAngleHorz;
        float dhorz = m_cam_recoil.YawRho * m_last_horz_step + (1.0f - m_cam_recoil.YawRho) * rdm * base_step;
        m_last_horz_step = dhorz;

        // Rotation angle from lean coupling, computed unconditionally (theta is 0 and the rotation a
        // no-op whenever LeanCoupling is 0).
        float theta = actor_roll * m_cam_recoil.LeanCoupling;
        if (!fis_zero(m_cam_recoil.LeanCoupling))
        {
            // Rotate the (vertical, horizontal) impulse this shot contributes by the actor's current
            // roll (lean) scaled by a tunable gain -- reproduces the yaw bias measured on Insurgency
            // footage when leaning, without hard-coding either engine's lean angle.
            float c = cosf(theta), s = sinf(theta);
            float ndvert = dvert * c - dhorz * s;
            dhorz = dvert * s + dhorz * c;
            dvert = ndvert;
        }

        m_angle_vert += dvert;
        clamp(m_angle_vert, -m_cam_recoil.MaxAngleVert, m_cam_recoil.MaxAngleVert);
        if (fis_zero(m_angle_vert - m_cam_recoil.MaxAngleVert))
            m_angle_vert *= m_Random.randF(0.96f, 1.04f);

        m_angle_horz += dhorz;
        clamp(m_angle_horz, -m_cam_recoil.MaxAngleHorz, m_cam_recoil.MaxAngleHorz);
    }
    else
    {
        // ---- stock path, byte-identical to the original code ----
        m_angle_vert += dvert;
        clamp(m_angle_vert, -m_cam_recoil.MaxAngleVert, m_cam_recoil.MaxAngleVert);
        if (fis_zero(m_angle_vert - m_cam_recoil.MaxAngleVert))
            m_angle_vert *= m_Random.randF(0.96f, 1.04f);

        float rdm = m_Random.randF(-1.0f, 1.0f);
        if (g_decouple_horz_recoil)
            m_angle_horz += rdm * m_cam_recoil.StepAngleHorz;
        else
            m_angle_horz += (m_angle_vert / m_cam_recoil.MaxAngleVert) * rdm * m_cam_recoil.StepAngleHorz;
        clamp(m_angle_horz, -m_cam_recoil.MaxAngleHorz, m_cam_recoil.MaxAngleHorz);
    }

    m_last_shot_impulse = _abs(dvert);
    m_first_shot = true;
    m_actived = true;
    m_shot_end = false;
}
```

At `YawRho = 0`, `dhorz` reduces exactly to `rdm * base_step` -- same magnitude formula as stock, just
without the accumulated-angle coupling.

`Relax()` is completely untouched (already linear-clamped, already matched measured footage).

`Update()` gains three additions, all gated on `InsurgencyRecoil`:

```cpp
// Continuous horizontal center-pull, independent of cam_return/Relax(). Without this, m_angle_horz is
// an unconstrained random walk during a sustained burst: per the arcsine law, a driftless random walk
// spends most of its time on whichever side it commits to early rather than crossing back through
// center, and YawRho's memory only reinforces that.
if (m_cam_recoil.InsurgencyRecoil && m_cam_recoil.YawCenterPull > 0.0f)
{
    const float pull_factor = expf(-m_cam_recoil.YawCenterPull * dt);
    m_angle_horz *= pull_factor;
    m_last_horz_step *= pull_factor; // also decay the AR(1) memory, or it keeps "reloading" against the pull
}

// Rise-time smoothing, vertical only (horizontal is a noisy, direction-reversing random walk --
// smoothing it produces a lag-then-snap ratchet at full-auto cadence, diagnosed 16/09).
if (m_cam_recoil.InsurgencyRecoil && m_cam_recoil.RiseTimeMs > 0.0f)
{
    ApproachRise(m_output_vert, m_angle_vert, m_cam_recoil.RiseTimeMs, dt);
    m_output_horz = m_angle_horz;
}
else
{
    m_output_vert = m_angle_vert;
    m_output_horz = m_angle_horz;
}
```

`ApproachRise` (local helper, anonymous namespace, top of `Update()`'s translation unit):

```cpp
// Eases 'out' toward 'target' over rise_time_ms whenever |target| is growing (a fresh kick); tracks
// 'target' exactly, instantly, whenever it's shrinking (Relax() already computed the correct shape).
void ApproachRise(float& out, float target, float rise_time_ms, float dt)
{
    if (rise_time_ms <= 0.0f || _abs(target) <= _abs(out))
    {
        out = target;
        return;
    }
    float rate = 1000.0f / rise_time_ms;
    float t = 1.0f - expf(-rate * dt);
    out += (target - out) * t;
}
```

`m_delta_vert`/`m_delta_horz` (consumed by `ChangeHP`/`GetDeltaAngle`) are now computed from
`m_output_vert`/`m_output_horz` instead of `m_angle_vert`/`m_angle_horz` directly -- transparent when
`RiseTimeMs == 0` since `m_output_* == m_angle_*` in that case.

### `src/xrGame/Actor_Weapon.cpp` -- `CActor::on_weapon_shot_start()`

Passes the actor's current camera roll into `Shot()` so lean coupling has something to rotate by:

```cpp
CameraRecoil const& camera_recoil = (IsZoomAimingMode()) ? weapon->zoom_cam_recoil : weapon->cam_recoil;
// ...
// Alt-aim override: some scopes tilt the view a fixed amount in a special aim mode where the actor's
// own Orientation().roll doesn't reflect the visible tilt (e.g. a canted red dot used as iron sights).
// insurgency_alt_aim_lean opts a weapon into substituting a fixed -45 degree roll for real roll while
// that specific alt-aim (GetZoomType() == 1) is active, so zoom_insurgency_lean_coupling biases the
// recoil the same way a real lean would.
float shot_actor_roll = (camera_recoil.InsurgencyRecoil && weapon->UseAltAimLeanCoupling() && weapon->GetZoomType() == 1)
    ? -PI_DIV_4
    : Orientation().roll;
effector->Shot(weapon, shot_actor_roll);

// Stock weapons keep the original fixed-magnitude cosmetic kick. Insurgency-recoil weapons instead
// scale it to the actual vertical kick this shot just applied, so Bodycam's cosmetic viewmodel motion
// tracks real per-shot variability instead of pulsing the same fixed amount every time.
float impulse_power = camera_recoil.InsurgencyRecoil ? effector->GetLastShotImpulse() : 1.f;
cam_BodycamAddFireImpulse(impulse_power);
```

`Orientation()` (returns `SRotation&`, `{yaw, pitch, roll}` in radians) already existed on `CActor` --
no new engine plumbing needed for the lean-roll read itself.

## `.ltx` keys (Part A)

All optional, all absent = stock behaviour, read in `CWeapon::Load()`:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `insurgency_recoil` | bool | `0` | Master opt-in switch for this whole system on this weapon. |
| `insurgency_yaw_rho` | float, ~0-1 | `0` | AR(1) memory on the horizontal step. `0` = stock-shape fresh random draw every shot (but still constant-amplitude, not accumulated-angle-coupled). |
| `insurgency_lean_coupling` | float | `0` | Gain on `actor_roll` when rotating the shot impulse. Sign/magnitude must be tuned empirically per engine (this engine: negative, see `.ltx` comments). |
| `insurgency_rise_time_ms` | float, ms | `0` | Ease-in time for the vertical kick. `0` = instant (stock). Measured real-world ballpark: 100-160ms. |
| `insurgency_muzzle_pivot` | float, ~0-2 | `0` | See Bodycam viewmodel section below (`recoil_pivot_y`/`_z`). |
| `insurgency_yaw_center_pull` | float, 1/s | `0` | Continuous exponential pull of the horizontal angle back to 0. `1.0` ~= 63%/s decay. |
| `insurgency_alt_aim_lean` | bool | `0` | Forces a fixed `-PI/4` roll substitute for lean coupling while a specific alt-aim mode is active. |
| `zoom_insurgency_yaw_rho` / `zoom_insurgency_lean_coupling` / `zoom_insurgency_rise_time_ms` / `zoom_insurgency_muzzle_pivot` / `zoom_insurgency_yaw_center_pull` | same as hip | inherits hip value | Optional ADS-only override for each field. Omit and ADS uses the same value as hip. |

## Bodycam viewmodel-follow side (for completeness)

Not part of the real-camera recoil system above, but consumes its output: `bodycam_simulation.cpp`
tracks `input.recoil_pitch`/`recoil_yaw` (fed from `GetOutputVert()`/`GetOutputHorz()`) into a
viewmodel-only "recoil follow" channel (`state.viewmodel.recoil_pos`/`recoil_rot`), independently
follow-speed/damping-tuned per axis, with its own ADS multiplier (`recoil_ads_mult`, NOT the shared
`ads_impulse_mult` -- see Part C for why that distinction matters) and its own muzzle-pivot redirection
(`MuzzlePivot`/`recoil_pivot_y`/`recoil_pivot_z`). This is pure Bodycam MCM-tunable cosmetic layer, no
further `.ltx` keys beyond `insurgency_muzzle_pivot` above.

---

# Part B -- Shot-progression animation tiers

## Files changed

### `src/xrGame/WeaponMagazined.h`

```cpp
// Insurgency-style shot-progression animation tiers (opt-in, insurgency_shot_anim_sustain in .ltx): shot
// 1 of a trigger pull plays anm_shots/anm_shots_aim, shot 2 plays its _second tier, shot 3 _third, shot
// 4+ plateaus on _fourth -- every shot still retriggers normally.
bool m_insurgencyShotAnimSustain; // protected member
```

### `src/xrGame/WeaponMagazined.cpp` -- `Load()`

```cpp
m_insurgencyShotAnimSustain = !!READ_IF_EXISTS(pSettings, r_bool, section, "insurgency_shot_anim_sustain", FALSE);
```

### `src/xrGame/WeaponMagazined.cpp` -- `PlayAnimShoot()`

```cpp
void CWeaponMagazined::PlayAnimShoot()
{
    VERIFY(GetState() == eFire);

    if (iAmmoElapsed > 1 || !HudAnimationExist("anm_shot_l"))
    {
        bool use_aim = IsZoomed() && HudAnimationExist("anm_shots_aim");
        shared_str anm_name = use_aim ? "anm_shots_aim" : "anm_shots";

        // Stock restarts anm_shots/anm_shots_aim from frame 0 on EVERY shot, hard-cut (PlayHUDMotion's
        // bMixIn2=false makes LL_PlayCycle always take the instant LL_CloseCycle path, never a fade --
        // verified in SkeletonAnimated.cpp/player_hud.cpp). At full-auto cadences faster than the clip's
        // own length only the opening ~3 frames are ever seen, and the clip's settle tail never plays.
        // With this on, every shot still retriggers exactly like stock (same cadence, no freeze/desync
        // risk -- each shot re-anchors to the real fire event), but WHICH clip family plays depends on
        // m_iShotNum: shot 1 uses anm_shots(_aim) as normal, shot 2 its _second tier, shot 3 _third, shot
        // 4+ plateaus on _fourth. Each tier is the same clip with frames 0..anchor-1 pulled progressively
        // closer to the anchor frame's own pose (the point where the next shot's hard-cut actually lands
        // at this RPM); the anchor frame onward is left untouched -- so consecutive hard-cuts land on
        // increasingly similar poses instead of popping back to a fully "unrecoiled" frame 0 every time.
        // The settle/decompensation tail then plays for free, unedited, on whichever tier is active the
        // moment nothing retriggers it (trigger release, jam, empty mag).
        if (m_insurgencyShotAnimSustain && m_iShotNum > 1)
        {
            LPCSTR tier = (m_iShotNum == 2) ? "_second" : (m_iShotNum == 3) ? "_third" : "_fourth";

            string128 tier_name;
            xr_sprintf(tier_name, "%s%s", anm_name.c_str(), tier);
            if (HudAnimationExist(tier_name))
                anm_name = tier_name;
        }

        PlayHUDMotion(anm_name, TRUE, this, GetState(), 1.f, 0.f, false);
    }
    else
    {
        if (!IsZoomed() || !HudAnimationExist("anm_shots_aim_l"))
            PlayHUDMotion("anm_shot_l", TRUE, this, GetState(), 1.f, 0.f, false);
        else
            PlayHUDMotion("anm_shots_aim_l", TRUE, this, GetState(), 1.f, 0.f, false);
    }
}
```

`m_iShotNum` is a pre-existing `CWeaponMagazined` member -- incremented before every `OnShot()`, reset to
`0` in `switch2_Idle()` **and** in `switch2_Fire()` (both reached via `OnStateSwitch`/`SwitchState`,
themselves only invoked from `FireStart()`/`OnAnimationEnd`). No new counter was added; this reuses the
counter that already drives dispersion-per-shot and condition wear.

**Critical: no new engine code was needed to pool multiple clip variants per tier.** X-Ray's own
`player_hud_motion_container::load()` (`player_hud.cpp`) already scans, for *every* `.ltx` key starting
with `anm_`, the model's own cycle list for `base_name`, `base_name+1` ... `base_name+8` and pools
whatever exists; `anim_play()` then picks a random one from that pool. So `anm_shots_second` (root
`h_uzi_shoot_second`) auto-discovers `h_uzi_shoot_second1/2/3` on its own, with zero extra `.ltx` keys
needed for the pooling itself -- the `..._variant1/2/3` keys below only exist as independently-addressable
aliases, not because the engine needs them to find the clips.

**Naming convention (load-bearing, not cosmetic):** tier suffix goes on the ROOT, the variant digit stays
LAST -- `h_uzi_shoot_second`, `h_uzi_shoot_second1`, `h_uzi_shoot_second2`, `h_uzi_shoot_second3`, **not**
`h_uzi_shoot1_second`. Only this ordering lets the engine's own `base_name+i` discovery find each tier's
own variant pool automatically.

## `.ltx` wiring (per weapon)

Only one new key on the weapon's main section:

```
insurgency_shot_anim_sustain = 1
```

Plus a `![wpn_XXX_hud]` additive patch (merges onto whatever the weapon's base `.ltx` already resolved --
verified directly in `Xr_ini.cpp::EvaluateSection`: an override's data is `MergeSections`'d onto the
base-resolved section, not a full replace, so existing `:hud_base` inheritance never needs repeating)
exposing each tier as a named `anm_shots_*` key -- **without these, `HudAnimationExist("anm_shots_second")`
returns false and `PlayAnimShoot()` silently falls back to vanilla every shot**, exactly as if the sustain
flag were off:

```
![wpn_uzi_hud]

    anm_shots_second               = h_uzi_shoot_second, uzi_shoot_second
    anm_shots_second_variant1      = h_uzi_shoot_second1, uzi_shoot_second1
    anm_shots_second_variant2      = h_uzi_shoot_second2, uzi_shoot_second2
    anm_shots_second_variant3      = h_uzi_shoot_second3, uzi_shoot_second3
    anm_shots_third                = h_uzi_shoot_third, uzi_shoot_third
    anm_shots_third_variant1       = h_uzi_shoot_third1, uzi_shoot_third1
    anm_shots_third_variant2       = h_uzi_shoot_third2, uzi_shoot_third2
    anm_shots_third_variant3       = h_uzi_shoot_third3, uzi_shoot_third3
    anm_shots_fourth               = h_uzi_shoot_fourth, uzi_shoot_fourth
    anm_shots_fourth_variant1      = h_uzi_shoot_fourth1, uzi_shoot_fourth1
    anm_shots_fourth_variant2      = h_uzi_shoot_fourth2, uzi_shoot_fourth2
    anm_shots_fourth_variant3      = h_uzi_shoot_fourth3, uzi_shoot_fourth3
    anm_shots_aim_second           = h_uzi_shoot_aim_second, uzi_shoot_second
    anm_shots_aim_second_variant1  = h_uzi_shoot_aim_second1, uzi_shoot_second1
    anm_shots_aim_second_variant2  = h_uzi_shoot_aim_second2, uzi_shoot_second2
    anm_shots_aim_second_variant3  = h_uzi_shoot_aim_second3, uzi_shoot_second3
    anm_shots_aim_third            = h_uzi_shoot_aim_third, uzi_shoot_third
    anm_shots_aim_third_variant1   = h_uzi_shoot_aim_third1, uzi_shoot_third1
    anm_shots_aim_third_variant2   = h_uzi_shoot_aim_third2, uzi_shoot_third2
    anm_shots_aim_third_variant3   = h_uzi_shoot_aim_third3, uzi_shoot_third3
    anm_shots_aim_fourth           = h_uzi_shoot_aim_fourth, uzi_shoot_fourth
    anm_shots_aim_fourth_variant1  = h_uzi_shoot_aim_fourth1, uzi_shoot_fourth1
    anm_shots_aim_fourth_variant2  = h_uzi_shoot_aim_fourth2, uzi_shoot_fourth2
    anm_shots_aim_fourth_variant3  = h_uzi_shoot_aim_fourth3, uzi_shoot_fourth3
```

Note the weapon-side (2nd column) name is shared between the hip and aim variants of a tier -- only the
HANDS-side (1st column) name differs -- matching how `anm_shots`/`anm_shots_aim` already share their
weapon-side name in the base config.

## Content pipeline: Blender addon

`insurgency_shot_tier_generator.py`, installed in Blender's addon folder (View3D > Sidebar >
"Insurgency Tiers" panel), automates generating all tier clips + exporting the OMFs from a single source
animation. Core algorithm (frame-compression toward an "anchor" frame):

```python
def suggested_anchor_frame(rpm, fps):
    """round(60/RPM * FPS) -- the frame where the NEXT shot's hard-cut actually lands at this fire rate."""
    if rpm <= 0:
        return 1
    return max(1, round((60.0 / rpm) * fps))

def compute_compressions(settings):
    """One compression fraction (0..1) per tier. DIMINISHING mode: 1 - 1/(k+1) for k=1,2,3,...
    -> 50%, 66.7%, 75%, 80%, ... (diminishing-returns approach to 100%, a damped-settling shape)."""
    n = settings.num_tiers
    if settings.compression_mode == 'DIMINISHING':
        return [1.0 - 1.0 / (i + 2) for i in range(n)]
    # LINEAR and CUSTOM modes also available -- see the addon file for the full branch.

def compress_action(action, anchor_frame, compression, new_name):
    """Pulls frames 0..anchor_frame-1 toward the anchor frame's own pose by `compression`
    (0=untouched, 1=identical to anchor), per fcurve, leaving the anchor frame and everything after it
    completely untouched. Only touches fcurves that have real keyframes at every frame 0..anchor_frame
    (skips static/idle bones automatically)."""
    for fc in action.fcurves:
        by_frame = {round(kp.co[0]): kp for kp in fc.keyframe_points}
        if not all(f in by_frame for f in range(0, anchor_frame + 1)):
            continue  # skip: this fcurve isn't keyed at every frame we need
        v_anchor = by_frame[anchor_frame].co[1]
        for f in range(0, anchor_frame):
            kp = by_frame[f]
            new_v = v_anchor + (1.0 - compression) * (kp.co[1] - v_anchor)
            delta = new_v - kp.co[1]
            kp.co[1] = new_v
            kp.handle_left[1] += delta
            kp.handle_right[1] += delta

def discover_digits(action_root):
    """Mirrors the engine's own base_name+0..8 discovery scan, so the addon only ever touches
    variant clips the engine would actually find on its own."""
    return [("" if i == 0 else str(i)) for i in range(0, 9)
            if bpy.data.actions.get(action_root + ("" if i == 0 else str(i))) is not None]

def ensure_bone_group(obj):
    """xray_export.omf_file errors on an armature with zero pose bone groups. Adding one minimal
    group containing every bone is purely organisational -- doesn't touch skinning/deform weights."""
    if len(obj.pose.bone_groups) == 0:
        grp = obj.pose.bone_groups.new(name="main")
        for pb in obj.pose.bones:
            pb.bone_group = grp
        return True
    return False
```

Inputs: hands armature (drives `wpn_body`'s `COPY_TRANSFORMS` constraint in this rig -- all visible
weapon recoil motion is actually driven by the HANDS armature's `lead_gun` bone, not the weapon's own),
weapon armature, comma-separated root action names, RPM, FPS, tier count, compression curve, tier
suffixes, output OMF paths. Exports via `bpy.ops.xray_export.omf_file(export_mode='OVERWRITE',
export_motions=True, export_bone_parts=False)`.

## `.ltx` keys (Part B)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `insurgency_shot_anim_sustain` | bool | `0` | Opt-in switch for this weapon. Silently falls back to vanilla `anm_shots`/`anm_shots_aim` if the tier `.ltx` keys or the underlying OMF motions don't exist -- no crash, no missing-animation risk. |

## Known limitation

`PlayAnimShoot()`'s tier selection is a hardcoded 3-tier ternary (`_second`/`_third`/`_fourth` plateau at
shot 4). A very high-RPM weapon wanting more than 4 distinct tiers would need this generalized to a
tier-count-driven loop/array lookup instead -- not done, since it wasn't needed for the weapons tuned so
far (the addon itself already supports generating more than 4 tiers, generic suffixes `_t2`/`_t3`/...;
only the C++ selector is capped).

---

# Part C -- Recoil decompensation (viewmodel-only "release" kick)

## Design constraints (why it's built this way)

- **No continuous mouse-input polling** -- must be a single event-driven kick, not a per-frame check.
- **Physically sound scaling** -- must NOT scale with the burst's *cumulative* recoil angle (which
  saturates against `cam_max_angle` well before a long burst ends, e.g. a 50-round drum vs. a 10-round
  mag reaching the same steady-state recoil should decompensate about the same amount, not wildly
  differently). Scales instead with the LAST shot's own kick (`GetLastShotImpulse()`), which itself
  plateaus at steady-state.
- **Viewmodel-only** -- never touches the real camera/aim.
- **Only fires after a real sustained burst**, not on every single shot or short tap.

## Trigger event

`CActor::on_weapon_shot_stop()`, called from `CWeapon::FireEnd()` -> `StopShotEffector()` ->
`inventory_owner().on_weapon_shot_stop()`, itself called directly and synchronously from
`CWeapon::Action(kWPN_FIRE, ...)` on the real key-release input event (not animation-gated, not polled)
-- gated by `effector->IsActive()` (only real, currently-accumulated recoil).

### `src/xrGame/Actor_Weapon.cpp`

```cpp
void CActor::on_weapon_shot_stop()
{
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    if (effector && effector->IsActive())
    {
        // Fires exactly once, right as a sustained burst genuinely ends. Scaled to the LAST shot's own
        // kick, not the burst's accumulated total. Gated on GetShotNumber()+1 (the actual number of
        // shots fired in the burst that just ended) against DecompMinShots (default 4): a single shot
        // or a short tap never built up any sustained climb worth "releasing".
        if (effector->IsInsurgencyRecoil() && effector->GetShotNumber() + 1 >= effector->GetDecompMinShots())
        {
            Bodycam::RecoilDecompOverride decomp_overrides;
            decomp_overrides.impulse = effector->GetDecompImpulse();
            decomp_overrides.vertical_scale = effector->GetDecompVerticalScale();
            decomp_overrides.forward_scale = effector->GetDecompForwardScale();
            decomp_overrides.pitch_scale = effector->GetDecompPitchScale();
            decomp_overrides.horizontal_scale = effector->GetDecompHorizontalScale();
            decomp_overrides.ads_scale = effector->GetDecompAdsScale();
            cam_BodycamAddRecoilDecompImpulse(effector->GetLastShotImpulse() * effector->GetDecompScale(), decomp_overrides);
        }

        effector->StopShoting();
    }
}
```

`GetShotNumber()` returns `m_shot_numer` (0-based, `weapon->ShotsFired() - 1`, i.e.
`WeaponMagazined::m_iShotNum - 1`) -- set every `Shot()` call, **not** cleared by `StopShoting()`, so it
still holds the just-ended burst's shot count at this exact point. Because `m_iShotNum` only resets on a
genuine `FireStart()` -> `switch2_Fire()` transition (which only happens when `bWorking` was `false`,
i.e. the trigger had actually been released, checked in `FireStart()`'s
`if (!IsWorking() || AllowFireWhileWorking())` gate), this correctly distinguishes a real sustained
full-auto hold from rapid press/release tapping (however fast) -- every genuine key-up resets the count
at the next key-down, so only an unbroken hold that reaches the threshold triggers decompensation.

### `src/xrGame/CameraRecoil.h` -- additions

```cpp
float DecompScale;       // Per-weapon multiplier (default 1) on top of the global MCM sliders --
                         // GetLastShotImpulse() already scales with this weapon's own recoil values,
                         // so 1 is "just use that"; this dials a specific weapon up/down beyond that
                         // (or exactly 0 to disable) without touching the global sliders.
int DecompMinShots;      // Minimum shots in the burst before decompensation fires at all. Default 4 --
                         // lines up with the anm_shots_fourth plateau tier from Part B.

// Per-weapon overrides for the 6 global "Decompensation" MCM sliders themselves. Each defaults to -1
// (kDecompUseGlobal sentinel), meaning "not set, inherit the global slider". Set any subset to shape
// THIS weapon's decompensation independently of every other InsurgencyRecoil weapon.
float DecompImpulse;
float DecompVerticalScale;
float DecompForwardScale;
float DecompPitchScale;
float DecompHorizontalScale;
float DecompAdsScale;
```

Defaults: `DecompScale(1.0f)`, `DecompMinShots(4)`, all 6 `Decomp*` override fields `(-1.0f)`. All added
to `Clone()`.

### `src/xrGame/Weapon.cpp` -- `CWeapon::Load()`

```cpp
cam_recoil.DecompScale = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_decomp_scale", 1.0f);
cam_recoil.DecompMinShots = READ_IF_EXISTS(pSettings, r_s32, section, "insurgency_decomp_min_shots", 4);
cam_recoil.DecompImpulse = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_decomp_impulse", -1.0f);
cam_recoil.DecompVerticalScale = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_decomp_vertical_scale", -1.0f);
cam_recoil.DecompForwardScale = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_decomp_forward_scale", -1.0f);
cam_recoil.DecompPitchScale = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_decomp_pitch_scale", -1.0f);
cam_recoil.DecompHorizontalScale = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_decomp_horizontal_scale", -1.0f);
cam_recoil.DecompAdsScale = READ_IF_EXISTS(pSettings, r_float, section, "insurgency_decomp_ads_scale", -1.0f);
```

All 8 fields cloned to `zoom_cam_recoil` right after (no separate `zoom_insurgency_decomp_*` override
mechanism was added -- ADS behaviour is already fully covered by `DecompAdsScale`/`ads_scale`).

### `src/xrGame/bodycam_simulation.h`

```cpp
constexpr float kDecompUseGlobal = -1.f;
struct RecoilDecompOverride
{
    float impulse = kDecompUseGlobal;
    float vertical_scale = kDecompUseGlobal;
    float forward_scale = kDecompUseGlobal;
    float pitch_scale = kDecompUseGlobal;
    float horizontal_scale = kDecompUseGlobal;
    float ads_scale = kDecompUseGlobal;
};

void AddRecoilDecompImpulse(const SimulationSettings& settings, SimulationState& state, float power,
    bool ads, const RecoilDecompOverride& overrides);
```

`SimulationImpulseSettings` (the global Bodycam config, MCM-bound) carries the 6 defaults this resolves
against:

```cpp
float recoil_decomp_impulse = 8.f;
float recoil_decomp_vertical_scale = 1.f;
float recoil_decomp_forward_scale = 1.f;
float recoil_decomp_pitch_scale = 1.f;
float recoil_decomp_horizontal_scale = 0.3f;
float recoil_decomp_ads_scale = 0.5f;
```

`SimulationViewmodelState` gains a **dedicated channel**, deliberately separate from the generic
`impulse_pos`/`impulse_rot`:

```cpp
// Recoil decompensation's own channel, kept separate from impulse_pos/rot on purpose: it must NOT be
// scaled by the shared viewmodel.ads_impulse_mult downstream (same reasoning as recoil_pos/rot using
// their own recoil_ads_mult instead of that shared multiplier) -- recoil_decomp_ads_scale is already
// applied once, at the moment the impulse is added, so this is the ONLY ADS attenuation it gets.
SVec3 decomp_pos;
SVec3 decomp_rot;
```

### `src/xrGame/bodycam_simulation.cpp`

```cpp
void AddRecoilDecompImpulse(const SimulationSettings& settings, SimulationState& state, float power,
    bool ads, const RecoilDecompOverride& overrides)
{
    if (!settings.features.vm_enable || !settings.features.fire_impulse_enable)
        return;

    const float base_impulse = overrides.impulse != kDecompUseGlobal ? overrides.impulse : settings.impulse.recoil_decomp_impulse;
    const float vertical_scale = overrides.vertical_scale != kDecompUseGlobal ? overrides.vertical_scale : settings.impulse.recoil_decomp_vertical_scale;
    const float forward_scale = overrides.forward_scale != kDecompUseGlobal ? overrides.forward_scale : settings.impulse.recoil_decomp_forward_scale;
    const float pitch_scale = overrides.pitch_scale != kDecompUseGlobal ? overrides.pitch_scale : settings.impulse.recoil_decomp_pitch_scale;
    const float horizontal_scale = overrides.horizontal_scale != kDecompUseGlobal ? overrides.horizontal_scale : settings.impulse.recoil_decomp_horizontal_scale;
    const float ads_scale_base = overrides.ads_scale != kDecompUseGlobal ? overrides.ads_scale : settings.impulse.recoil_decomp_ads_scale;

    const float ads_scale = ads ? Clamp(ads_scale_base, 0.f, 1.f) : 1.f;
    const float p = Clamp(power, 0.f, 3.f) * base_impulse * ads_scale;
    if (p <= kEpsilon)
        return;

    // Internal convention x=pitch, y=yaw, z=roll. Anti-rise pitch is the OPPOSITE sign from a normal
    // upward recoil kick -- the muzzle dips instead of climbing, right as the compensating force the
    // shooter was applying loses what it was fighting.
    state.viewmodel.decomp_pos.Add(
        horizontal_scale * 0.006f * p,
        -vertical_scale * 0.010f * p,
        -forward_scale * 0.010f * p);
    state.viewmodel.decomp_rot.Add(
        -pitch_scale * 0.9f * p,
        0.f,
        horizontal_scale * 0.4f * p);

    ClampVector(state.viewmodel.decomp_pos, std::max(settings.impulse.impulse_pos_cap, 0.f));
    ClampVector(state.viewmodel.decomp_rot, std::max(settings.impulse.impulse_rot_cap, 0.f));
}
```

`AddNamedImpulse` (the generic Lua-facing `kind`-string dispatcher, used for `"fire"`/`"land"`/`"sprint_*"`
/`"ads_*"`) gets a `"recoil_decomp"` case that calls the same function with a default-constructed
(all-sentinel) `RecoilDecompOverride` -- so `bodycam.add_impulse("recoil_decomp", power)` from Lua still
works, just without per-weapon overrides (no weapon context available through that generic path).

**Decay + composition** (in the main `Update()` function): `decomp_pos`/`decomp_rot` are clamped and
decayed in the same `if (fire_impulse_enabled)` block as `fire_impulse_pos`/`fire_impulse_rot` (same
exponential decay formula, same `settings.impulse.decay` rate), then added directly into
`vm_pos_target`/`vm_rot_target` **without** the `ads_impulse_mult` multiply that `vm_impulse_pos`/
`vm_impulse_rot` get:

```cpp
SVec3 vm_pos_target = vm_mouse_pos;
vm_pos_target.Add(vm_impulse_pos);           // this one IS scaled by ads_impulse_mult, above
vm_pos_target.Add(state.viewmodel.decomp_pos); // NOT scaled -- ads_scale (above) is its only ADS attenuation
SVec3 vm_rot_target = vm_mouse_rot;
vm_rot_target.Add(vm_impulse_rot);
vm_rot_target.Add(state.viewmodel.decomp_rot);
```

**Why this separation matters (a real bug that was fixed):** the shared `impulse_pos`/`impulse_rot`
channel is globally attenuated by `ads_impulse_mult` (default `0.18`) while aiming. If decompensation
had stayed on that channel, its own `ads_scale` (default `0.5`) would have **compounded** with the
global multiplier (`0.5 x 0.18 ~= 0.09`), making it almost invisible in ADS regardless of how the
weapon's own `ads_scale` was tuned. Giving it a dedicated channel (mirroring how `recoil_pos`/`recoil_rot`
already avoid that same shared multiplier via their own `recoil_ads_mult`) makes `ads_scale` the single,
predictable ADS control for this effect.

Also reset in `ResetSimulation()` and the `hud_transform_enabled == false` fallback branch, alongside
every other viewmodel channel.

### `src/xrGame/bodycam_camera.h` / `.cpp`

```cpp
// .h
void AddRecoilDecompImpulse(float power, bool ads, const RecoilDecompOverride& overrides);

// .cpp
void CBodycam::AddRecoilDecompImpulse(float power, bool ads, const RecoilDecompOverride& overrides)
{
    Bodycam::AddRecoilDecompImpulse(GetSimulationSettings(), m_state, power, ads, overrides);
}
```

(Qualified with the `Bodycam::` namespace prefix to disambiguate from this same-named member function --
same pattern already used by the pre-existing `AddFireImpulse`.)

### `src/xrGame/Actor.h` / `ActorCameras.cpp`

```cpp
// Actor.h
void cam_BodycamAddRecoilDecompImpulse(float power, const Bodycam::RecoilDecompOverride& overrides);

// ActorCameras.cpp
void CActor::cam_BodycamAddRecoilDecompImpulse(float power, const Bodycam::RecoilDecompOverride& overrides)
{
    if (this == Level().CurrentEntity())
        m_bodycam.AddRecoilDecompImpulse(power, GetBodycamAdsState(*this).active, overrides);
}
```

### `src/xrGame/bodycam_settings.cpp` -- MCM bindings

```cpp
BODYCAM_FLOAT(vm_recoil_decomp_impulse, impulse.recoil_decomp_impulse, 0.f, 30.f),
BODYCAM_FLOAT(vm_recoil_decomp_vertical_scale, impulse.recoil_decomp_vertical_scale, 0.f, 3.f),
BODYCAM_FLOAT(vm_recoil_decomp_forward_scale, impulse.recoil_decomp_forward_scale, 0.f, 3.f),
BODYCAM_FLOAT(vm_recoil_decomp_pitch_scale, impulse.recoil_decomp_pitch_scale, 0.f, 3.f),
BODYCAM_FLOAT(vm_recoil_decomp_horizontal_scale, impulse.recoil_decomp_horizontal_scale, 0.f, 3.f),
BODYCAM_FLOAT(vm_recoil_decomp_ads_scale, impulse.recoil_decomp_ads_scale, 0.f, 1.f),
```

`settings.impulse = g_bodycam_config.impulse;` (whole-struct copy in `GetSimulationSettings()`) already
propagated these with no further change needed.

### Lua/MCM script -- `options_modded_exes_bodycam_recoil.script`

6 new `bodycam_num` sliders in the existing "Bodycam Weapon Recoil" MCM tab, same pattern as every other
slider in that file:

```lua
bodycam_num { id = "vm_recoil_decomp_impulse", text = label("vm_recoil_decomp_impulse"), def = 8.0, min = 0.0, max = 30.0, step = 0.5, prec = 1, key = "vm_recoil_decomp_impulse" },
bodycam_num { id = "vm_recoil_decomp_vertical_scale", text = label("vm_recoil_decomp_vertical_scale"), def = 1.0, min = 0.0, max = 3.0, step = 0.05, prec = 2, key = "vm_recoil_decomp_vertical_scale" },
bodycam_num { id = "vm_recoil_decomp_forward_scale", text = label("vm_recoil_decomp_forward_scale"), def = 1.0, min = 0.0, max = 3.0, step = 0.05, prec = 2, key = "vm_recoil_decomp_forward_scale" },
bodycam_num { id = "vm_recoil_decomp_pitch_scale", text = label("vm_recoil_decomp_pitch_scale"), def = 1.0, min = 0.0, max = 3.0, step = 0.05, prec = 2, key = "vm_recoil_decomp_pitch_scale" },
bodycam_num { id = "vm_recoil_decomp_horizontal_scale", text = label("vm_recoil_decomp_horizontal_scale"), def = 0.3, min = 0.0, max = 3.0, step = 0.05, prec = 2, key = "vm_recoil_decomp_horizontal_scale" },
bodycam_num { id = "vm_recoil_decomp_ads_scale", text = label("vm_recoil_decomp_ads_scale"), def = 0.5, min = 0.0, max = 1.0, step = 0.05, prec = 2, key = "vm_recoil_decomp_ads_scale" },
```

Labels added to `gamedata/configs/text/eng/st_bodycam_mcm.xml` (e.g. "Decompensation Intensity",
"Decompensation Downward Dip", "Decompensation Forward Push", "Decompensation Anti-Rise",
"Decompensation Sideways Sway", "Decompensation ADS Scale").

## `.ltx` keys (Part C)

| Key | Type | Default | Overrides |
|---|---|---|---|
| `insurgency_decomp_scale` | float, ~0-3 | `1.0` (inherits base) | Multiplies the final computed power (`GetLastShotImpulse() * DecompScale`). |
| `insurgency_decomp_min_shots` | int, >=1 | `4` | Minimum shots in the burst before decompensation fires at all. |
| `insurgency_decomp_impulse` | float, 0-30 | `-1` = global | Replaces "Decompensation Intensity" slider. |
| `insurgency_decomp_vertical_scale` | float, 0-3 | `-1` = global | Replaces "Decompensation Downward Dip" slider. |
| `insurgency_decomp_forward_scale` | float, 0-3 | `-1` = global | Replaces "Decompensation Forward Push" slider. |
| `insurgency_decomp_pitch_scale` | float, 0-3 | `-1` = global | Replaces "Decompensation Anti-Rise" slider. |
| `insurgency_decomp_horizontal_scale` | float, 0-3 | `-1` = global | Replaces "Decompensation Sideways Sway" slider. |
| `insurgency_decomp_ads_scale` | float, 0-1 | `-1` = global | Replaces "Decompensation ADS Scale" slider. Own dedicated ADS channel -- `1.0` here means full hip-strength decompensation in ADS too. |
| `zoom_insurgency_decomp_scale` | float | inherits hip | Optional ADS-only override for `insurgency_decomp_scale` specifically. |

`insurgency_decomp_scale` (a multiplier on the *input power*) and the 6 `insurgency_decomp_*`
shape/intensity overrides (which *replace* specific coefficients) are independent, stacking knobs --
using both together is fine, not redundant, since they act at different points in the formula.

---

## Backward compatibility (all three parts)

Every new `.ltx` key defaults to a value that reproduces the exact original stock behaviour when absent.
No existing weapon, no existing script, no existing save is affected by these changes unless its `.ltx`
explicitly sets one of the new keys. Verified by construction (every new field's default traces back to
either "off"/`false`, `0`, or the pre-existing stock constant) and by regression-testing an unmigrated
weapon (AK) feeling identical before/after.

## Known limitations / possible follow-ups

- Shot-tier selection is a hardcoded 3-tier ternary (see Part B) -- a generalized N-tier version would
  need `PlayAnimShoot()` reworked to index an array/loop instead.
- No `zoom_insurgency_decomp_impulse`/`_vertical_scale`/etc. override mechanism -- ADS shaping is
  currently only controllable via `insurgency_decomp_ads_scale` (a single intensity multiplier for ADS),
  not per-axis-shape-in-ADS.
- `insurgency_lean_coupling`'s sign is engine/rig-dependent (verified empirically in-game, not derivable
  from code alone without reading further render code) -- documented per-weapon in the `.ltx` comments,
  not a fixed universal constant.
- The AR(1) yaw-memory coefficient and lean-coupling gain were tuned by feel against a modest, noisy
  signal in the reference footage (Insurgency: Sandstorm) -- not a precise physical model, treat
  `insurgency_yaw_rho`/`insurgency_lean_coupling` as "system exists, values are a starting point" rather
  than validated constants.

## Testing status

Compiled clean (`DX11-AVX|x64`, `xrEngine` target, 0 errors, only pre-existing unrelated warnings).
Tested in-game: shot-tier animations (UZI, full-auto, before/after comparison via
`insurgency_shot_anim_sustain=0/1`), decompensation min-shots gating (single shot / short tap -> no
kick; sustained 4+ shot burst -> one kick on release), decompensation ADS channel (previously
near-invisible in ADS due to double attenuation, fixed by the dedicated channel), per-weapon
decompensation overrides (UZI tuned with `insurgency_decomp_impulse=29`,
`insurgency_decomp_vertical_scale=3.0`, etc., independent of the global MCM sliders, confirmed working
even with the global sliders set to 0).
