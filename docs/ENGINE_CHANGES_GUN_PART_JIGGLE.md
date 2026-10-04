# Engine change: secondary-motion ("jiggle") physics for weapon-attached moving parts

Status: implemented and compiled (`DX11-AVX|x64`, `xrEngine`/`xrGame` targets, 0 errors, 0 warnings from
the new code). Deployed, not yet tested in-game. First pass (v1) -- deliberately scoped down from the
full design discussed in chat; see "Known limitations" for what's intentionally deferred.

## Motivation

Animation-driven weapon parts (a bipod, a carry handle, a keychain) can't easily get continuous, reactive
motion from the animation system alone -- animations have a fixed length and don't blend arbitrarily with
real-time inputs (recoil, mouse movement, sprinting) without a full blending rig, which is a much bigger
undertaking. This adds a lightweight damped-spring secondary-motion layer instead: a small number of
explicitly declared bones on a weapon's own HUD model lag behind the weapon's actual motion and settle
back, the same category of effect as hair/cloth/antenna jiggle in other engines.

## Research before implementation

Before writing any code, investigated whether `https://github.com/Zira3l137/xray-monolith-lass` (a
third-party fork with its own "jiggle bones" feature) could be reused directly. Findings, from reading
its actual source (not just its README):

- LASS's `bone_spring.h/.cpp` targets the **actor's own body skeleton** (bust bones by default), hooked
  through `CActor::SetCallbacks()`/`OnChangeVisual()`. The underlying engine mechanism it rides on
  (`bctCustom` bone callbacks via `IKinematics::LL_GetBoneInstance().set_callback()`) is generic --
  already used in this codebase on several different skeleton types (actor, hands fingers, cars,
  monsters, AI, `script_attachment_manager`) -- but the `CBoneSpringController` class itself is tied to
  `CActor`/the equipped outfit's `.ltx`, not reusable as-is for a weapon's own skeleton.
- LASS is built on `xray-monolith-pip`, not on this fork's base (`asuparabekon/xray-monolith-bodycam`) --
  a sibling fork, not an ancestor. A direct merge/cherry-pick was never on the table; the only thing
  actually reused is the *technique* (damped-spring-per-bone math, frame-hitch/teleport guards, the
  swing-via-cross-product trick for turning linear lag into a bone rotation).
  `src/xrGame/weapon_part_jiggle.cpp`'s header comment documents this explicitly.
- Searched this fork's own code for a weapon-skeleton precedent before writing anything new:
  `bodycam_hud_arms.cpp` already does procedural per-bone posing (direct `mTransform`/`mRenderTransform`
  writes) on the **hands** skeleton's arm bones, driven by `bodycam_simulation`'s `SimulationOutput` --
  proof that "read the simulation state, pose a bone" is an idiom this fork already uses, just not yet on
  the weapon's own mesh. That technique needs an explicit call at the right point in the frame, though,
  and the weapon's own `CalculateBones()` is never called explicitly from `xrGame` (it's invoked by the
  renderer when the weapon's dynamic visual is submitted) -- so the LASS-style automatic callback
  (`bctCustom`), not the hud-arms explicit-write style, is the correct mechanism specifically for weapon
  bones. Confirmed by reading `player_hud.cpp` in full rather than assuming either way.
- Confirmed the driving signal: `attachable_hud_item::m_item_transform` (the weapon's own per-frame root
  transform, read by `player_hud.cpp` right before `DM->add_Dynamic(m_model->dcast_RenderVisual(),
  &m_item_transform)`) is derived from the hand bone's finalized pose, which already has Bodycam's full
  `GetHudOffset()` composite (recoil + sway + decompensation + mouse-aim) folded in before the hands'
  own `CalculateBones()` runs. So tracking `m_item_transform` frame-to-frame -- the same
  position-delta-to-pseudo-acceleration technique LASS uses for the actor's whole-body jump/landing
  response -- captures all of those sources for free, with no bespoke per-source wiring.

## Summary

- **Per-bone, not shared-global, tuning.** Every spring/constraint parameter lives in
  `SJiggleBoneConfig`, read into each bone's own `SJiggleBoneState` at install time -- two bones on the
  same weapon can have entirely different physics. (LASS's reference design uses shared globals across
  all its bones; explicitly not carried over, per the user's own requirement that e.g. a keychain and a
  carry handle on the same weapon could need different physics.)
- **Two independent rotation axes, not one isotropic swing cone.** The reference design derives its
  rotation axis automatically (`cross(length_axis, lag)`) and clamps only the overall lag magnitude, so a
  part can swing in any direction perpendicular to its length axis up to that one shared budget. This
  implementation instead takes that same cross product and projects it onto up to two explicitly
  configured hinge axes (`jiggle_pitch_axis`/`jiggle_roll_axis`), each with its own gain and hard
  `max_deg` limit -- a literal "how far can this part fold" / "how far can it roll" pair, independently
  tunable, which is what actually keeps a part from swinging into geometry it shouldn't. Composed box-style
  (independent clamps), not a coupled cone -- see "Known limitations" for why.
- **Config lives in a standalone test mod**, not in the engine repo's own `gamedata` -- same pattern as
  `INSURGENCY_RECOIL_TEST_MOD`/`NEAR_FADE_TEST_MOD`: a `mod_system_gun_parts_physics.ltx` using DLTX
  `![section]` overrides, so it adds `jiggle_bones` to a weapon's HUD section without touching or
  requiring the weapon pack's own files.
- **Off by default, costs nothing for any item that doesn't declare `jiggle_bones`.** No `.ltx` key, no
  bones found, no callback installed, no per-frame work at all for that item.

## Files changed

### `src/xrGame/weapon_part_jiggle.h` / `.cpp` (new)

`SJiggleAxisLimit` (axis + gain + max_deg + active flag), `SJiggleBoneConfig` (stiffness/damping/
bone_length/world_gain/translate_gain/translate_max + the two axis limits), `SJiggleBoneState` (owner
pointer back to the controller, config, spring integration state), `CGunPartJiggleController`
(`install`/`remove`, up to `MAX_JIGGLE_BONES = 4` per item).

`install(attachable_hud_item* owner, const shared_str& weapon_section)`: reads `jiggle_bones` (format
`bone_name:config_section`, comma-separated) from the item's own HUD section, resolves each bone name via
`K->LL_BoneID()`, skips (with a log line) any bone that doesn't exist on this model or already has a
callback -- same defensive pattern as the reference design. Reads each bone's own config section
(`jiggle_stiffness`, `jiggle_damping`, `jiggle_bone_length`, `jiggle_world_gain`,
`jiggle_translate_gain`/`_max`, `jiggle_pitch_axis`/`_gain`/`_max_deg`, `jiggle_roll_axis`/`_gain`/
`_max_deg`) via `pSettings`, same idiom as every other `.ltx`-driven feature in this fork. Installs
`bctCustom` callbacks only once the whole array is built (pointers handed to callbacks must stay stable).

`update_world_accel()`: tracks `owner->m_item_transform` frame-to-frame (position delta -> velocity ->
acceleration, teleport/frame-hitch guarded exactly like the reference design), rotates the result into the
weapon's own model space via `m_item_transform`'s own basis vectors. This is the one per-controller signal
shared by every bone on that item this frame (cached, computed once via a frame-number guard).

`integrate()`: per-bone damped-spring step (target = the bone's own currently-animated local position,
same `bi->mTransform.c` read the reference design uses), sub-stepped at a fixed 120Hz up to 8 steps/frame,
driven by both the spring's own restoring force and the shared world-space pseudo-force. Produces
`applied_lag`, a raw displacement vector -- not yet clamped to either axis's own limit, since that
happens per-axis in `apply()`.

`apply()`: projects `applied_lag` into the bone's local frame, computes `cross(forward, local_lag)` (the
reference design's own rotation-axis derivation), then for each active hinge axis takes that cross
product's component along the configured axis as a signed angle, clamps it to that axis's own `max_deg`,
and composes the (up to two) resulting rotations via `mul_43` (bone-local, origin-preserving, same
composition rule as the reference design). Translation keeps a simple isotropic clamp
(`translate_max`), applied on top.

`bone_callback()`: same frame-dedup pattern as the reference design (`CalculateBones` can run more than
once per frame; integrate once, re-apply the cached result on any repeat call, since `BuildBoneMatrix`
rebuilds the animated pose from scratch every call).

### `src/xrGame/player_hud.h`

`attachable_hud_item` gains `CGunPartJiggleController m_jiggle;`.

### `src/xrGame/player_hud.cpp`

- `attachable_hud_item::load()`: `m_jiggle.install(this, sect_name)` at the end, once `m_model` and the
  measures are loaded -- `sect_name` here is the item's **HUD** section (e.g. `wpn_hk21_hud`, which has
  `item_visual`), not the weapon's main item/ammo section (`wpn_hk21`, which doesn't).
- `attachable_hud_item::~attachable_hud_item()`: `m_jiggle.remove()` before `m_model` is deleted.

### `src/xrGame/console_commands.cpp`

```cpp
CMD4(CCC_Integer, "g_gunjiggle_enabled", &g_gunjiggle_enabled, 0, 1); // master kill switch, default on
CMD4(CCC_Integer, "g_gunjiggle_debug", &g_gunjiggle_debug, 0, 1);    // logs install success/failure per item
```

Per-bone tuning itself is `.ltx`-only in v1, not console-live -- see "Known limitations".

### `src/xrGame/xrGame.vcxproj` / `.vcxproj.filters`

New files registered (plain `ClCompile`, default `stdafx.h` PCH, same as e.g. `HudSound.cpp`/`Wound.cpp`),
filed under the same `Core\Client\Objects\items & weapons\HudItem` folder as `player_hud.cpp`.

### `F:\GAMMA_V095\mods\GUN_PARTS_PHYSICS_TEST_MOD\gamedata\configs\mod_system_gun_parts_physics.ltx` (new)

DLTX override on `wpn_hk21_hud` adding `jiggle_bones = biji1:wpn_hk21_bipod_leg, biji2:wpn_hk21_bipod_leg`
(both legs share one config section -- mechanically identical, symmetric around the same pivot ball) plus
the `[wpn_hk21_bipod_leg]` section itself with starting-point values for every parameter above.

## Backward compatibility

No existing weapon has a `jiggle_bones` key. `install()` returns immediately if the key is absent, or if
none of the listed bones resolve on that model. No per-frame cost, no behavior change, for any item that
doesn't opt in.

## Known limitations / deferred by design (discussed in chat before implementation)

- **Single driving signal, not per-source weighted blending.** v1 feeds the spring from the weapon's
  whole composite motion (`m_item_transform`'s own acceleration) with one `jiggle_world_gain`. The
  discussed refinement -- separately weighting `bodycam_simulation`'s own `recoil_pos/rot`,
  `sway_pos/rot`, `sprint.amount` channels before summing into the spring's target, so e.g. recoil and
  idle sway can drive the same bone with different relative strength -- was deliberately deferred until
  this base version is seen moving in-game. The data needed for it already exists in `SimulationOutput`;
  adding it later doesn't require re-architecting anything above.
- **Independent per-axis clamps ("box"), not a coupled cone.** Being near `jiggle_pitch_max_deg` doesn't
  currently reduce how much roll is simultaneously available. Discussed and deliberately deferred: for
  motion this subtle, a box and a true coupled cone are expected to look the same in practice; only worth
  the extra complexity if testing shows a visibly-impossible combined pose.
- **Axis directions in the test `.ltx` are a first guess**, not verified against the actual Blender rig.
  If the swing happens in the wrong plane, swap which key (`jiggle_pitch_axis`/`jiggle_roll_axis`) owns
  which world/bone axis, or flip its sign -- no engine change needed, `.ltx`-only.
- **No live console tuning per bone** (unlike the reference design's global `g_jiggle_*` cvars) --
  because tuning is per-bone/per-section by design, a console-exposed equivalent would need a currently-
  selected-bone concept that doesn't exist yet. Workflow for now: edit the `.ltx`, holster/draw the
  weapon to re-trigger `load()` (confirmed this reloads config -- no restart needed), repeat.
- **No real collision/mesh-interpenetration avoidance**, only amplitude clamping (`translate_max`,
  `max_deg` per axis) -- same approach the reference design uses. Relies on conservative tuning rather
  than geometric constraint-checking; revisit only if clamped amplitude alone proves insufficient.
- **`attachable_hud_item::load()` re-entrancy not stress-tested.** `install()` defensively calls
  `remove()` first (handles being called twice on the same, still-valid object), but whether `load()` is
  ever called a second time on an object whose `m_model` has already changed underneath it wasn't traced
  all the way through weapon-swap code -- flagged here rather than asserted either way.

## Testing status

Compiled clean (`xrGame` target standalone: 0 errors, 0 warnings from the new files; full `xrEngine` link:
0 errors). Deployed (binary + test mod). Not yet run in-game -- next step is equipping the HK21 with the
test mod active and checking `g_gunjiggle_debug 1`'s install-confirmation log line, then whether the bipod
visibly reacts to recoil/movement at all before any axis/amplitude tuning.
