# Engine change: sound occlusion instrumentation (Phase 0 -- measurement only)

Status: implemented and compiled (`DX11-AVX|x64`, `xrEngine` target, 0 errors), not yet measured in-game.

Context: first step of a larger, not-yet-started occlusion rework. Full plan/rationale in
`OCCLUSION_ENGINE_PATCH.md` and `OCCLUSION_EXPLAINED.md` (same folder). **This change implements only
Phase 0 of that plan**: pure measurement, zero behaviour change. Phases 1-4 (material-based occlusion,
diffraction, indoor/outdoor second reverb, corridor propagation) are not started.

## Summary

Adds timed counters around the engine's existing sound occlusion code (unchanged logic, unchanged
values) so its real per-frame cost (time, ray count, calls, how many sounds get occluded) can be
measured before anything about it changes. Exposed two ways:

- `rs_stats 1` + `snd_occlusion_stats 1`: two new `OCC:`/`OCC AI:` lines in the existing sound stats
  overlay (`*** SOUND:` block), showing the *last fully-completed frame's* cost.
- A `* [snd_occ] ...` summary line written to the log every ~5 seconds while `snd_occlusion_stats 1`,
  averaged/maxed over that window (avg ms/frame, max ms, calls/s, rays/s, % blocked, 3D emitters/frame,
  plus the AI-hearing side separately).

`snd_occlusion_stats 0` (the default): the counters still update (negligible cost -- a handful of
integer increments and one QPC read/write pair per call, already how every other timer in this engine
works) but nothing is displayed or logged, and no summary math runs.

## Motivation

Before touching the occlusion system itself (a much larger change -- material-aware losses, a low-pass
filter, diffraction around obstacles, indoor/outdoor second reverb), the actual cost of the *current*
system needs a real baseline. Guessing "it's probably cheap, it's just one ray" isn't good enough to
know whether a heavier replacement is affordable, or which specific scenes (a firefight with 6+ NPCs,
long sightlines in Garbage, corridor combat in Jupiter Underground) are actually expensive. A future
rework needs the same counters anyway, to prove it isn't *more* expensive than what it replaces (see
"points d'attention" in `OCCLUSION_ENGINE_PATCH.md`: "mode 0 doit être strictement identique").

No script (Lua) can measure this -- occlusion runs entirely inside native C++ in the sound thread's main
update loop (`CSoundRender_Core::update()`), never through a call a Lua profiler could wrap. Confirmed
by inspecting the two profiling mods on hand (`JIT_PROFILER`, `ANO_DEVTOOLS`): both work by wrapping Lua
functions (LuaBind method profiling, auto-discovered Lua modules) -- neither can see inside
`get_occlusion()`/`update_culling()`. The counters below had to be native.

## What the existing system actually does (verified against this fork's source)

| Step | Code | Behaviour |
|---|---|---|
| Geometry tested | `IGame_Level.cpp:124` -- `Sound->set_geometry_occ(ObjectSpace.GetStaticModel())` | The level's entire **static** collision model (terrain, buildings, rocks, any static object with collision). |
| Test | `CSoundRender_Core::get_occlusion` -- `SoundRender_Core_Processor.cpp:259-334` | **One ray**, listener to a random point in a 0.2m sphere around the source. A cached triangle (the last obstacle) is tested first (`CDB::TestRayTri`); otherwise a full `OPT_ONLYNEAREST` query against the static model. |
| Result | same | Hit -> `occ_value = psSoundOcclusionScale` (`system.ltx [sound] occlusion_scale`, clamped 0.1-0.5; **GAMMA runs 0.4 -> -8dB**). **No material read at all.** Then, if the level has a `.som` occlusion mesh, multiplied by each crossed polygon's stored factor (`OPT_CULL` query, all hits). |
| When | `CSoundRender_Emitter` -- `SoundRender_Emitter_FSM.cpp`, 3 call sites: `stStarting`/`stStartingLooped` (lines 132, 157 -- assigned directly, no smoothing) and `update_culling`'s 3D branch (line 403 -- **every frame**, for every non-`world_ambient` 3D emitter within `max_distance`) | Smoothed via `volume_lerp(occluder_volume, occ, 1.f, dt)`, i.e. ~1.0/s. |
| Applied | `update_culling`: `smooth_volume *= occluder_volume` (line 411) | Gain only, **no filter**. The engine already loads the EFX low-pass filter functions (`SoundRender_CoreA.cpp`) but never uses them for occlusion -- `AL_FILTER_NULL` throughout. |
| 2D sounds | `update_culling`, `b2D` branch | `occluder_volume` forced to `1.f` -- never occluded (player's own HUD weapon, stereo files, UI, music). |
| AI hearing | `IGame_Level::SoundEvent_Register` -> `get_occlusion_to` (`SoundRender_Core_Processor.cpp:221`) | Uses **only** the `.som` mesh, never the static model. Player and AI do not hear the same occlusion. |
| Unused material data | `SGameMtl::fSndOcclusionFactor` (`GameMtlLib.h`), settable per material via `gamedata\materials\*.ltx` (`sound_occlusion_factor`) | Exists, Lua-exposed (`material_snd_occlusion_factor`), **ignored** by the player-side occlusion path. |

Matches `OCCLUSION_ENGINE_PATCH.md` section 1 exactly -- every file/line reference in that table was
independently re-checked against this fork's actual current source before writing any code.

## Files changed

Six files, all additive (nothing about the table above changes -- only what gets measured).

### `src/xrSound/Sound.h`

New console-bound global, next to the existing `psSoundOcclusionScale`:

```cpp
XRSOUND_API extern int psSoundOcclusionStats;
```

`CSound_stats` (the struct `Stats.cpp` already reads via `::Sound->statistic(&snd_stat, 0)`) gains 7
fields:

```cpp
class XRSOUND_API CSound_stats
{
public:
    u32 _rendered;
    u32 _simulated;
    u32 _cache_hits;
    u32 _cache_misses;
    u32 _events;

    float _occ_ms;       // player-side occlusion cost, last completed frame
    u32   _occ_calls;
    u32   _occ_rays;
    u32   _occ_blocked;
    float _occ_ai_ms;    // AI-hearing side (get_occlusion_to)
    u32   _occ_ai_calls;
    u32   _emitters_3d;  // active 3D emitters this frame (context for the above)
};
```

### `src/xrSound/SoundRender_Core.h`

`CSoundRender_Core` gains public counter state (public, not friend-gated, matching this class's existing
style -- `s_emitters`, `Timer`, etc. are already public):

```cpp
struct SOcclusionCounters
{
    u64 ticks = 0, ticks_ai = 0;
    u32 calls = 0, rays = 0, blocked = 0, calls_ai = 0, emitters_3d = 0;
    void reset() { *this = SOcclusionCounters(); }
};
SOcclusionCounters m_occ_cur;    // frame currently being built
SOcclusionCounters m_occ_last;   // last fully-completed frame -- what statistic() reports
SOcclusionCounters m_occ_window; // rolling accumulator for the ~5s log summary
u32 m_occ_window_frames = 0;
float m_occ_window_time = 0.f;
u64 m_occ_window_max_ticks = 0;
```

### `src/xrSound/SoundRender_Core_Processor.cpp`

A small RAII scope timer (file-local, same idea as the engine's other QPC-based timers, e.g.
`FTimer.h`):

```cpp
namespace
{
struct occ_scope_timer
{
    u64 t0;
    u64& acc;
    occ_scope_timer(u64& a) : t0(CPU::QPC()), acc(a) {}
    ~occ_scope_timer() { acc += CPU::QPC() - t0; }
};
}
```

`get_occlusion()` and `get_occlusion_to()` each gain a timer + call counter at their top, and
`get_occlusion()` gets a ray-query counter at each of its two real `ray_query()` call sites (the cached
`TestRayTri` fast path is deliberately NOT counted as a ray -- it's a single cached-triangle test, not a
database query) plus a `blocked` counter right before its single `return`:

```cpp
float CSoundRender_Core::get_occlusion(Fvector& P, float R, Fvector* occ)
{
    occ_scope_timer _t(m_occ_cur.ticks);
    m_occ_cur.calls++;
    float occ_value = 1.f;
    // ... unchanged body ...
    //   m_occ_cur.rays++ added right before each of the two geom_DB.ray_query(...) calls
    if (occ_value < 1.f)
        m_occ_cur.blocked++;
    return occ_value;
}
```

`statistic()` copies `m_occ_last` into the `CSound_stats*` output (ticks -> ms via `CPU::qpc_freq`),
alongside the existing `_rendered`/`_simulated`/etc. copies it already does.

`update()`, right after `fTimer_Value = new_tm;` (where `fTimer_Delta` first becomes available this
frame): closes out the previous frame's counters into `m_occ_last`, resets `m_occ_cur` for the frame
about to run, and -- only if `psSoundOcclusionStats` -- accumulates into the 5-second window and prints
the summary once the window fills:

```cpp
m_occ_last = m_occ_cur;
m_occ_cur.reset();
if (psSoundOcclusionStats)
{
    // accumulate m_occ_last into m_occ_window, track m_occ_window_max_ticks
    m_occ_window_time += fTimer_Delta;
    if (m_occ_window_time >= 5.f)
    {
        Msg("* [snd_occ] avg %.3fms/frame  max %.3fms  calls/s %.0f  rays/s %.0f  blocked %.0f%%  "
            "3D emitters/frame %.1f  |  AI %.3fms/frame (%.0f calls/s)", /* ... */);
        // reset the window
    }
}
```

Because this runs at the very top of `update()`, `m_occ_last` reflects one fully-completed frame and
stays stable for `statistic()` to read at any point until the next `update()` call -- at most one frame
of latency on the displayed numbers, not noticeable for a debug overlay.

### `src/xrSound/SoundRender_Emitter_FSM.cpp`

One counter in `update_culling()`'s 3D (`else`/non-`b2D`) branch, right at entry -- counts every active
3D emitter reaching this branch this frame, regardless of whether it goes on to actually call
`get_occlusion` (a `world_ambient` sound or one already past `max_distance` won't), as context for
interpreting `calls`/`rays`:

```cpp
SoundRender->m_occ_cur.emitters_3d++;
```

### `src/xrEngine/Stats.cpp`

Two new lines in the `rs_stats 1` sound block, right after the existing `HIT/MISS:` line, gated on
`psSoundOcclusionStats`:

```cpp
if (psSoundOcclusionStats)
{
    F.OutNext("  OCC:       %2.3fms, %d calls, %d rays, %d blocked, %d 3D",
        snd_stat._occ_ms, snd_stat._occ_calls, snd_stat._occ_rays, snd_stat._occ_blocked, snd_stat._emitters_3d);
    F.OutNext("  OCC AI:    %2.3fms, %d calls", snd_stat._occ_ai_ms, snd_stat._occ_ai_calls);
}
```

### `src/xrEngine/xr_ioc_cmd.cpp`

Registered next to the existing `snd_efx_*` commands, same idiom, saved to `user.ltx` like any other
console variable:

```cpp
CMD4(CCC_Integer, "snd_occlusion_stats", &psSoundOcclusionStats, 0, 1);
```

## Backward compatibility

Nothing about occlusion's actual behaviour changes -- every line of the original `get_occlusion`/
`get_occlusion_to`/`update_culling` logic is untouched; only timing/counting statements were inserted
around it. `psSoundOcclusionStats` defaults to `0`: no display, no log line, no summary math. The
counters themselves keep incrementing regardless (they're cheap -- see below), but nothing reads or
prints them unless the console command is turned on.

## Points of attention

1. **Overhead of the counters themselves**: one `CPU::QPC()` call pair (RAII timer) and a few integer
   increments per `get_occlusion`/`get_occlusion_to` call, already the exact mechanism every other timed
   stat in this engine uses (`Device.Statistic`, `FTimer`, etc.). Expected to be unmeasurable next to the
   ray queries themselves; Phase 0's own baseline run will confirm this empirically rather than assume it.
2. **`m_occ_window` accumulates only while `psSoundOcclusionStats` is on** -- turning it on mid-session
   starts a fresh window, doesn't retroactively include earlier frames.
3. **`emitters_3d` vs `calls`**: intentionally different things. `emitters_3d` counts every active 3D
   emitter reaching `update_culling`'s non-2D branch; `calls` only counts emitters that actually invoked
   `get_occlusion` (excludes `world_ambient` and anything already past `max_distance`). The gap between
   the two is itself informative (how much culling before occlusion is even attempted).
4. **No `snd_occlusion_mode` yet** -- that's the Phase 1 switch (`OCCLUSION_ENGINE_PATCH.md` section 4.1).
   This change has no mode toggle because there's nothing to toggle yet; the log summary line omits a
   mode field for the same reason (would always read 0).
5. **AI-hearing side has no ray counter**, only calls/ms -- `get_occlusion_to`'s single `ray_query` call
   is always exactly one per call when `geom_SOM` exists, so a separate ray count would be redundant
   with `_occ_ai_calls`.

## Test plan (baseline measurement, per `OCCLUSION_ENGINE_PATCH.md` section 3.5)

1. `rs_stats 1` + `snd_occlusion_stats 1`, log file open.
2. Same save, 5 fixed scenes, 30-60s each: quiet outdoor; 6+ NPC firefight; Garbage hills (NPC behind a
   rise); Jupiter Underground corridors; an anomaly-dense area (many looped sounds).
3. Record per scene: `avg ms/frame`, `max ms`, `rays/s`, `% blocked`, `3D emitters/frame`, plus the
   existing `*** SOUND` total and FPS.
4. Repeat each scene once to check the numbers are stable (not a fluke of that particular run).
5. These numbers become the reference baseline for every later phase -- each phase's own test plan
   re-runs the same 5 scenes and compares.

## Known limitations / next steps

- This is measurement only. It answers "how expensive is occlusion today", not "make it better" --
  that's Phases 1-4, not started, not scoped for compilation until Phase 0's numbers are in and reviewed.
- No Optick integration yet (`OCCLUSION_ENGINE_PATCH.md` mentions `PROF_EVENT("Sound: occlusion")` as an
  optional deeper option, for a frame-by-frame timeline instead of the periodic average/max here) --
  skipped for this pass since the `rs_stats`/log summary is enough to establish a baseline number.

## Testing status

Compiled clean (`DX11-AVX|x64`, `xrEngine` target, 0 errors, only pre-existing unrelated warnings).
Not yet run in-game -- pending the 5-scene baseline measurement pass above.
