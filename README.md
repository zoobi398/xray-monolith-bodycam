# X-Ray Monolith Bodycam MT -- fade audio + Insurgency-style recoil + sound occlusion + gun-part jiggle + cyclic gunfire voice + HUD animation layers

Personal fork of [asuparabekon/xray-monolith-bodycam](https://github.com/asuparabekon/xray-monolith-bodycam)
(itself the X-Ray Monolith MT + Bodycam camera/viewmodel + objective-camera PiP scope build). This
fork's own additions, on top of everything already in the upstream build:

1. **Configurable per-emitter audio fade-out/fade-in** for 2D sound sources (`ref_sound::set_fade_out`/
   `set_fade_in`, Lua-exposed) -- lets a script crossfade between two sound instances with a duration/
   curve it controls, instead of the engine's old fixed ~100ms linear fade. Full writeup:
   [`docs/ENGINE_CHANGES_FADE_SYSTEM.md`](docs/ENGINE_CHANGES_FADE_SYSTEM.md).
2. **Insurgency-style recoil, shot-progression animation tiers, and recoil decompensation** -- an
   opt-in (per-weapon, `.ltx`-gated), fully backward-compatible overhaul of the camera recoil system:
   short-term-memory horizontal kick with optional lean coupling, animation clips that get visibly less
   "snappy" across a sustained full-auto burst instead of hard-cutting to the same frame every shot, and
   a one-shot viewmodel "release" kick right as a burst genuinely ends, and an optional symmetric low-pass on the horizontal recoil
   (`insurgency_yaw_smooth_ms`) that turns the per-shot sideways "staircase" into a continuous glide. Full writeup, with every changed
   function and every new `.ltx` key: [`docs/ENGINE_CHANGES_INSURGENCY_RECOIL.md`](docs/ENGINE_CHANGES_INSURGENCY_RECOIL.md).
3. **Material-aware sound occlusion with real diffraction, plus actor-fire priority ducking** --
   `snd_occlusion_mode 1` replaces the original single-ray flat `-8dB` occlusion with a per-material-class
   system (foliage/glass/metal/wood/masonry/terrain), multi-sample averaging, a full Fresnel/Maekawa
   diffraction model for over/around-obstacle paths, and a real EFX low-pass filter (duller, not just
   quieter, behind cover) -- independently tunable direct vs. reverb-send attenuation, console-measured
   performance instrumentation, and an MCM page. `snd_duck_mode 1` adds a temporary, loudness-weighted
   gain reduction on NPC gunshot voices while the actor is firing, approximating auditory masking so the
   actor's own shot doesn't get squashed by OpenAL's output limiter when both overlap. Both default off
   (byte-identical to the original engine otherwise). Full writeups:
   [`docs/ENGINE_CHANGES_SOUND_OCCLUSION_PHASE0.md`](docs/ENGINE_CHANGES_SOUND_OCCLUSION_PHASE0.md)
   (measurement/instrumentation only), [`docs/ENGINE_CHANGES_SOUND_OCCLUSION_PHASE1_2.md`](docs/ENGINE_CHANGES_SOUND_OCCLUSION_PHASE1_2.md)
   (the rework itself, including the optional indoor value set driven by the Spatial Audio Rework indoor
   score), [`docs/ENGINE_CHANGES_ACTOR_FIRE_DUCKING.md`](docs/ENGINE_CHANGES_ACTOR_FIRE_DUCKING.md).
4. **Secondary-motion ("jiggle") physics for weapon-attached parts** -- a per-bone damped-spring
   controller for moving parts on a weapon's own model (bipod legs, carry handles, keychains), driven by
   the weapon's own motion (so it reacts to Bodycam's recoil, sway and movement for free), with
   independent per-bone tuning and per-axis hinge limits, configured entirely from `.ltx`. Opt-in per
   weapon (`jiggle_bones`), costs nothing for any weapon that doesn't declare it. Experimental: first
   version, being tuned in-game. Full writeup: [`docs/ENGINE_CHANGES_GUN_PART_JIGGLE.md`](docs/ENGINE_CHANGES_GUN_PART_JIGGLE.md).
5. **Near-fade for layered NPC gunshot sounds** -- an optional 4th/5th field on a world sound's `.ltx` line makes a distant-only
   layer (`snd_X_layer`) skip playback, and fade in with distance, instead of playing at full volume when the NPC is close.
   Opt-in per sound line. Writeup: [`docs/ENGINE_CHANGES_NEAR_FADE.md`](docs/ENGINE_CHANGES_NEAR_FADE.md).
6. **Cyclic gunfire in the engine (native audio voice)** -- the actor's `custom_loop_sound` weapons (start / looped blocks / end
   samples, as used by the TRUE CYCLIC sound mod) can be played by a native voice with its own OpenAL sources: block-exact looping
   that follows the real shot interval, an end sample that starts on a block boundary, and AI-hearing events for every shot.
   `snd_cyclic_native 1` to enable (default 0). First cut, being tested. Writeup: [`docs/ENGINE_CHANGES_CYCLIC_GUNFIRE_IN_ENGINE.md`](docs/ENGINE_CHANGES_CYCLIC_GUNFIRE_IN_ENGINE.md).
7. **HUD animation layers** -- independent additive animation layers on a weapon's own model (belt movement, carry handle, ...),
   started by shots, burst start / end, HUD animation names, motion marks or Lua, with their own duration, never cut by the main
   animation. Opt-in per weapon (`hud_layers`). Writeup and animator guide: [`docs/ENGINE_CHANGES_HUD_ANIM_LAYERS.md`](docs/ENGINE_CHANGES_HUD_ANIM_LAYERS.md).

Everything else -- the Bodycam camera/viewmodel system, objective-camera true PiP scopes, MCM menus,
build/install instructions, modder integration guide -- is upstream `asuparabekon` work, documented in
full at [`docs/PIP_SYSTEM.md`](docs/PIP_SYSTEM.md) (the original README, moved here so this one can
stay focused on what this fork actually changes).

## All additions are opt-in and backward-compatible

None of these features touch anything unless explicitly turned on (a weapon's `.ltx` for recoil, a
script call for fade, a console command/MCM toggle for occlusion and ducking), and none are used by any
existing content in this repository by default (the fade API's only caller is an external gameplay mod;
the recoil system is off for every weapon unless `insurgency_recoil`/`insurgency_shot_anim_sustain` is
set; `snd_occlusion_mode` and `snd_duck_mode` both default to `0`; `snd_cyclic_native` defaults to `0`; HUD animation layers
exist only for a weapon whose HUD section declares `hud_layers`). A stock weapon, a stock sound, or a
build of this repo that never touches the new `.ltx` keys/console commands behaves identically to
upstream. See each doc's "Backward compatibility" section for how that's verified.

## Building

Same as upstream -- Visual Studio 2022, Desktop development with C++, submodules initialized
recursively:

```powershell
git clone --recursive https://github.com/zoobi398/xray-monolith-bodycam.git
```

Open `src/engine-vs2022.sln`, build `DX11-AVX | x64` (or `DX11 | x64`), copy the resulting executable +
PDB from `_build/_game/bin_dbg` into your Anomaly/GAMMA `bin` folder alongside this repo's `gamedata`
installed as its own MO2 mod. See [`docs/PIP_SYSTEM.md`](docs/PIP_SYSTEM.md#installing-a-matched-build)
for the full install/compatibility notes (3DSS patches, shader cache, etc.) -- unrelated to the two
features documented here, but required for this build to run correctly at all.

## Trying the recoil/sway features in GAMMA

The engine changes are opt-in and need `.ltx`/script content to turn them on for specific weapons; that
content isn't bundled in this repo's `gamedata` (it's GAMMA-weapon-pack-specific, not part of the base
engine mod). A ready-to-install example mod (UZI + Kriss Vector wired up, plus the weapon sway/
hold-breath/arm-injury scripts) is attached to the
[latest release](https://github.com/zoobi398/xray-monolith-bodycam/releases/latest) as
`INSURGENCY_RECOIL_SWAY_MOD.zip`:

1. Download the zip from the release page above.
2. Install it as a normal MO2 mod (drag the zip onto MO2's mod list, or extract into
   `MO2/mods/INSURGENCY_RECOIL_SWAY_MOD/`).
3. Give it a **high priority** in the MO2 mod order -- above the weapon packs it patches (it uses DLTX
   `![section]` overrides, so it only needs to win on the specific keys it adds).
4. Requires this engine build. It's inert (no effect, no crash) on a stock executable.

The mod's own `README.md` and `RECOIL_TUNING_GUIDE.md` (inside the zip) cover every `.ltx` key and how
to extend it to other weapons.

## Credits

- [asuparabekon/xray-monolith-bodycam](https://github.com/asuparabekon/xray-monolith-bodycam) -- the
  base this fork builds on (Bodycam camera/viewmodel, objective-camera true PiP);
- [X-Ray Monolith](https://github.com/themrdemonized/xray-monolith) and its contributors;
- the original PiP work in the [gc64 fork](https://github.com/CnRJay/xray-monolith-gc64);
- Insurgency: Sandstorm (Focus Entertainment / New World Interactive) -- source reference footage for
  the recoil system's target feel.

## License

This repository inherits the upstream X-Ray engine licensing terms. See [License.txt](License.txt).
The original S.T.A.L.K.E.R. engine code is available for non-commercial use under its applicable terms.
