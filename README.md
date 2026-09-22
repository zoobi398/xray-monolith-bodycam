# X-Ray Monolith Bodycam MT -- fade audio + Insurgency-style recoil

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
   a one-shot viewmodel "release" kick right as a burst genuinely ends. Full writeup, with every changed
   function and every new `.ltx` key: [`docs/ENGINE_CHANGES_INSURGENCY_RECOIL.md`](docs/ENGINE_CHANGES_INSURGENCY_RECOIL.md).

Everything else -- the Bodycam camera/viewmodel system, objective-camera true PiP scopes, MCM menus,
build/install instructions, modder integration guide -- is upstream `asuparabekon` work, documented in
full at [`docs/PIP_SYSTEM.md`](docs/PIP_SYSTEM.md) (the original README, moved here so this one can
stay focused on what this fork actually changes).

## Both additions are opt-in and backward-compatible

Neither feature touches anything unless a weapon's `.ltx` explicitly asks for it, and neither is used
by any existing content in this repository (the fade API's only caller is an external gameplay mod; the
recoil system is off for every weapon unless `insurgency_recoil`/`insurgency_shot_anim_sustain` is set).
A stock weapon, a stock sound, or a build of this repo that never touches the new `.ltx` keys behaves
identically to upstream. See each doc's "Backward compatibility" section for how that's verified.

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
