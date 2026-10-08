#ifndef SoundH
#define SoundH
#pragma once


#ifdef XRSOUND_EXPORTS
#define XRSOUND_API
//__declspec(dllexport)
#else
	#define XRSOUND_API
//__declspec(dllimport)
#endif

#ifdef __BORLANDC__
	#define XRSOUND_EDITOR_API XRSOUND_API

	// editor only refs
	class XRSOUND_EDITOR_API SoundEnvironment_LIB;
#else
#define XRSOUND_EDITOR_API
#endif

#define SNDENV_FILENAME				"sEnvironment.xr"
#define OGG_COMMENT_VERSION 		0x0003

// refs
class CObject;
class XRSOUND_API CSound_params;
class XRSOUND_API CSound_source;
class XRSOUND_API CSound_emitter;
class XRSOUND_API CSound_stream_interface;
class XRSOUND_API CSound_environment;

XRSOUND_API extern u32 psSoundModel;
XRSOUND_API extern float psSoundVEffects;
XRSOUND_API extern float psSoundVFactor;
XRSOUND_API extern float psSoundVMusic;
XRSOUND_API extern float psSoundVMusicFactor;
XRSOUND_API extern float psSoundRolloff;
XRSOUND_API extern float psSoundOcclusionScale;
// Phase 0 occlusion instrumentation (29/09): "snd_occlusion_stats" console command. Off by default --
// purely adds the OCC:/OCC AI: lines to rs_stats and a periodic log summary, changes nothing about
// occlusion itself. See docs/ENGINE_CHANGES_SOUND_OCCLUSION.md.
XRSOUND_API extern int psSoundOcclusionStats;

// Phase 1/2 occlusion rework (29/09). psSoundOcclusionMode 0 (default) is the untouched, byte-identical
// original path above (psSoundOcclusionScale etc.); 1 switches 3D emitters to the material-aware,
// multi-sample, diffraction-capable path in get_occlusion_ex(). Everything below is inert at mode 0.
XRSOUND_API extern int psSoundOcclusionMode;
XRSOUND_API extern float psSoundOcclusionStrength;   // snd_occlusion_strength: global dB multiplier, 0-2
XRSOUND_API extern int psSoundOcclusionUpdateMs;     // snd_occlusion_update_ms: recalc interval for long/looped sounds
XRSOUND_API extern int psSoundOcclusionBudget;       // snd_occlusion_budget: max full evaluations per frame
XRSOUND_API extern int psSoundOcclusionDiffraction;  // snd_occlusion_diffraction: 0/1, over/around candidate paths
XRSOUND_API extern int psSoundOcclusionDebug;        // snd_occlusion_debug: 0 off, 1 = per-shot log line
// snd_occlusion_wet_sensitivity (30/09): how much the reverb SEND path follows the direct path's
// occlusion loss. 0 = the room's reflected energy is never attenuated by occlusion at all (only SAR's
// own indoor/room-size reverb governs it); 1 = fully coupled, the old (reported too "on/off") behaviour
// where ducking behind cover chokes the reverb send exactly as hard as the direct sound.
XRSOUND_API extern float psSoundOcclusionWetSensitivity;

// Indoor occlusion set (02/10). Not a second material table: only the global strength and the two hard
// caps differ between outdoor and indoor, interpolated by psSoundOcclusionIndoorFactor (0 = outdoor
// values, 1 = indoor values). The factor is pushed by gamedata/scripts/sound_occlusion_indoor.script from
// the actor's Spatial Audio Rework indoor score; with psSoundOcclusionIndoorMode 0 (default) the factor is
// ignored and behaviour is identical to before this existed. Split of responsibilities: the caps (outdoor
// AND indoor) are acoustic data in sound_occlusion.ltx [sound_occlusion_limits]; strength (outdoor
// snd_occlusion_strength, indoor snd_occlusion_indoor_strength) and the mode switch are menu/console.
XRSOUND_API extern int psSoundOcclusionIndoorMode;        // snd_occlusion_indoor_mode: 0/1
XRSOUND_API extern float psSoundOcclusionIndoorFactor;    // snd_occlusion_indoor_factor: 0..1, script-driven
XRSOUND_API extern float psSoundOcclusionIndoorStrength;  // snd_occlusion_indoor_strength

// Actor-fire priority ducking (30/09, "snd_duck_mode"): a temporary gain reduction on NPC gunshot voices
// while the actor's own weapon is firing, approximating auditory masking (a shot at your own ear dominates
// simultaneous perception) instead of letting both compete for the same OpenAL Soft output-limiter
// headroom. Off by default -- inert, byte-identical to before this existed, until enabled.
XRSOUND_API extern int psSoundDuckMode;           // snd_duck_mode: 0 off (default), 1 on
XRSOUND_API extern float psSoundDuckStrength;     // snd_duck_strength: 0-1, max gain cut on a full-loudness NPC shot
XRSOUND_API extern float psSoundDuckHoldMs;       // snd_duck_hold_ms: how long the duck stays engaged after the actor's last shot
XRSOUND_API extern float psSoundDuckAttackRate;   // snd_duck_attack_rate: gain units/s approaching the ducked target
XRSOUND_API extern float psSoundDuckReleaseRate;  // snd_duck_release_rate: gain units/s releasing back to 1.0

// Cyclic gunfire in the engine (doc 08_ENGINE_CHANGES_CYCLIC_GUNFIRE_IN_ENGINE.md). Off by default: with
// snd_cyclic_native 0 (or on a weapon without custom_loop_sound) nothing below is ever used and the
// scripted TRUE CYCLIC system keeps working exactly as before.
XRSOUND_API extern int psSoundCyclicNative;       // snd_cyclic_native: 0 off (default), 1 on
XRSOUND_API extern int psSoundCyclicDebug;        // snd_cyclic_debug: 0 off, 1 = log every cyclic event
XRSOUND_API extern float psSoundCyclicPipelineMs; // snd_cyclic_pipeline_ms: constant delay between a shot and its sound (absorbs frame jitter)

// One resolved material class (see sound_occlusion.ltx). loss_db/hf_loss_db apply once per crossed
// surface; loss_db_per_m only applies when a matching entry+exit pair of the SAME material is found on
// the same ray (real measured thickness) -- a single unpaired hit never guesses at a thickness.
struct SSoundOcclusionMaterial
{
	float loss_db = 10.f;
	float loss_db_per_m = 1.f;
	float hf_loss_db = 12.f;
	bool ignore = false; // acoustically transparent (foliage, thin grass...) -- contributes nothing
};
XRSOUND_API extern Flags32 psSoundFlags;
XRSOUND_API extern int psSoundTargets;
XRSOUND_API extern float snd_efx_environment_change_time;
XRSOUND_API extern float psSpeedOfSound;
XRSOUND_API extern int psSoundCacheSizeMB;
XRSOUND_API extern xr_token* snd_devices_token;
XRSOUND_API extern xr_string snd_device_name;

// reverb overwrite
extern BOOL reverb_overwrite;

extern float psReverbDensity;
extern float psReverbDiffusion;
extern float psReverbGain;
extern float psReverbGainHF;
extern float psReverbGainLF;
extern float psReverbDecayTime;
extern float psReverbDecayHFRatio;
extern float psReverbDecayLFRatio;
extern float psReverbReflectionsGain;
extern float psReverbReflectionsDelay;
extern float psReverbReflectionsPan;
extern float psReverbLateReverbGain;
extern float psReverbLateReverbDelay;
extern float psReverbLateReflectionsPan;
extern float psReverbEchoTime;
extern float psReverbEchoDepth;
extern float psReverbModulationTime;
extern float psReverbModulationDepth;
extern float psReverbAirAbsorptionGainHF;
extern float psReverbHFReference;
extern float psReverbLFReference;
extern float psReverbRoomRolloffFactor;
extern BOOL   psReverbDecayHFLimit;

// Flags
enum
{
	ss_Hardware = (1ul << 1ul),
	//!< Use hardware mixing only
	ss_EFX = (1ul << 2ul),
	//!< Use eax
	ss_forcedword = u32(-1)
};

enum
{
	sq_DEFAULT,
	sq_NOVIRT,
	sq_LIGHT,
	sq_HIGH,
	sq_forcedword = u32(-1)
};

enum
{
	sg_Undefined = 0,
	sg_SourceType = u32(-1),
	sg_forcedword = u32(-1),
};

enum
{
	sm_Looped = (1ul << 0ul),
	//!< Looped
	sm_2D = (1ul << 1ul),
	//!< 2D mode
	sm_Intro = (1ul << 2ul), //!< Only for music and video
	sm_forcedword = u32(-1),
};

enum esound_type
{
	st_Effect = 0,
	st_Music = 1,
	st_forcedword = u32(-1),
};

class CSound_UserDataVisitor;

class CSound_UserData : public xr_resource
{
public:
	virtual ~CSound_UserData()
	{
	}

	virtual void accept(CSound_UserDataVisitor*) =0;
	virtual void invalidate() =0;
};

typedef resptr_core<CSound_UserData, resptr_base<CSound_UserData>> CSound_UserDataPtr;

class ref_sound_data : public xr_resource
{
public:
	//	shared_str						nm;
	CSound_source* handle; //!< Pointer to wave-source interface
	CSound_emitter* feedback; //!< Pointer to emitter, automaticaly clears on emitter-stop
	esound_type s_type;
	int g_type; //!< Sound type, usually for AI
	CObject* g_object; //!< Game object that emitts ref_sound
	CSound_UserDataPtr g_userdata;
	shared_str fn_attached [2];

	u32 dwBytesTotal;
	float fTimeTotal;
public:
	ref_sound_data();
	ref_sound_data(LPCSTR fName, esound_type sound_type, int game_type);
	virtual ~ref_sound_data();
	float get_length_sec() const { return fTimeTotal; };
};

typedef resptr_core<ref_sound_data, resptr_base<ref_sound_data>> ref_sound_data_ptr;

/*! \class ref_sound
\brief Sound source + control

The main class respresenting source/emitter interface
This class infact just hides internals and redirect calls to 
specific sub-systems
*/
struct ref_sound
{
	ref_sound_data_ptr _p;
public:
	ref_sound()
	{
	}

	~ref_sound()
	{
	}

	IC CSound_source* _handle() const { return _p ? _p->handle : NULL; }
	IC CSound_emitter* _feedback() { return _p ? _p->feedback : 0; }
	IC CObject* _g_object()
	{
		VERIFY(_p);
		return _p->g_object;
	}

	IC int _g_type()
	{
		VERIFY(_p);
		return _p->g_type;
	}

	IC esound_type _sound_type()
	{
		VERIFY(_p);
		return _p->s_type;
	}

	IC CSound_UserDataPtr _g_userdata()
	{
		VERIFY(_p);
		return _p->g_userdata;
	}

	IC void create(LPCSTR name, esound_type sound_type, int game_type);
	IC void attach_tail(LPCSTR name);

	IC void clone(const ref_sound& from, esound_type sound_type, int game_type);

	IC void destroy();

	IC void play(CObject* O, u32 flags = 0, float delay = 0.f);
	IC void play_at_pos(CObject* O, const Fvector& pos, u32 flags = 0, float delay = 0.f);
	IC void play_no_feedback(CObject* O, u32 flags = 0, float delay = 0.f, Fvector* pos = 0, float* vol = 0,
	                         float* freq = 0, Fvector2* range = 0);

	IC void stop();
	IC void stop_deffered();
	IC void set_fade_out(float duration_s, int curve = 0);
	IC void set_fade_in(float duration_s, int curve = 0);
	IC void set_position(const Fvector& pos);
	IC void set_frequency(float freq);
	IC void set_range(float min, float max);
	IC void set_volume(float vol);
	IC void set_priority(float vol);

	IC const CSound_params* get_params();
	IC void set_params(CSound_params* p);
	IC float get_length_sec() const { return _p ? _p->get_length_sec() : 0.0f; };
};

/// definition (Sound Source)
class XRSOUND_API CSound_source
{
public:
	virtual float length_sec() const = 0;
	virtual u32 game_type() const = 0;
	virtual LPCSTR file_name() const = 0;
	virtual u16 channels_num() const = 0;
	virtual u32 bytes_total() const = 0;
};

/// definition (Sound Source)
class XRSOUND_API CSound_environment
{
public:
};

namespace soundSmoothingParams {
	extern float pitchVariationPower;
	extern float distanceBasedDelayPower;
	extern float distanceBasedDelayMinDistance;
	extern float power;
	extern int steps;
	extern float alpha;
	extern float getAlpha();
	extern float getTimeDeltaSmoothing();
	extern float getSmoothedValue(float, float, float);
};

/// definition (Sound Params)
class XRSOUND_API CSound_params
{
public:
	CSound_params() :
		set(false)
		{
			position.set(0.0f, 0.0f, 0.0f);
			velocity.set(0.0f, 0.0f, 0.0f);
			accVelocity.set(0.f, 0.f, 0.f);

			// demonized: add pitch variation
			pitch_variation = 0.02 * Random.randF(-1.f, 1.f) * soundSmoothingParams::pitchVariationPower;
		}

private:
	bool set;

public:
	Fvector position;
	Fvector velocity;  // Cribbledirge.  Added for doppler effect.
	Fvector curVelocity;  // Current velocity.
	Fvector prevVelocity;  // Previous velocity.
	Fvector accVelocity;  // Velocity accumulator (for moving average).
	float base_volume;
	float volume;
	float freq;
	float min_distance;
	float max_distance;
	float max_ai_distance;

	float pitch_variation;

	// Functions added by Cribbledirge for doppler effect.
	IC virtual void update_position(const Fvector& newPosition)
	{
		// If the position has been set already, start getting a moving average of the velocity.
		if (set)
		{
			prevVelocity.set(accVelocity);
			curVelocity.sub(newPosition, position);

			//accVelocity.set(curVelocity.mul(alpha).add(prevVelocity.mul(1.f - alpha)));
		}
		else
		{
			set = true;
		}
		position.set(newPosition);
	}

	IC virtual void update_velocity(const float dt)
	{
		float a = soundSmoothingParams::getTimeDeltaSmoothing();
		int p = soundSmoothingParams::power;
		accVelocity.x = soundSmoothingParams::getSmoothedValue(curVelocity.x * p / dt, accVelocity.x, a);
		accVelocity.y = soundSmoothingParams::getSmoothedValue(curVelocity.y * p / dt, accVelocity.y, a);
		accVelocity.z = soundSmoothingParams::getSmoothedValue(curVelocity.z * p / dt, accVelocity.z, a);
		velocity.set(accVelocity);

		//Msg("VELOC: %f", velocity.magnitude());
	}
};

/// definition (Sound Interface)
class XRSOUND_API CSound_emitter
{
public:
	virtual BOOL is_2D() = 0;
	virtual void switch_to_2D() = 0;
	virtual void switch_to_Intro() = 0;
	virtual void switch_to_3D() = 0;
	virtual void set_position(const Fvector& pos) = 0;
	virtual void set_frequency(float freq) = 0;
	virtual void set_range(float min, float max) = 0;
	virtual void set_volume(float vol) = 0;
	virtual void set_priority(float vol) = 0;
	virtual void stop(BOOL bDeffered) = 0;
	virtual void set_fade_out(float duration_s, int curve) = 0;
	virtual void set_fade_in(float duration_s, int curve) = 0;
	virtual const CSound_params* get_params() = 0;
	virtual u32 play_time() = 0;
};

/// definition (Sound Stream Interface)
class XRSOUND_API CSound_stream_interface
{
public:
};

/// definition (Sound Stream Interface)
class XRSOUND_API CSound_stats
{
public:
	u32 _rendered;
	u32 _simulated;
	u32 _cache_hits;
	u32 _cache_misses;
	u32 _events;

	// Phase 0 occlusion instrumentation (29/09): cost of the last fully-completed frame's occlusion
	// work, gated behind psSoundOcclusionStats. All zero if the flag is off (no measurable overhead
	// either way -- see CSoundRender_Core::SOcclusionCounters). "_ai" is the AI-hearing side
	// (get_occlusion_to), the rest is the player-side (get_occlusion).
	float _occ_ms;
	u32 _occ_calls;
	u32 _occ_rays;
	u32 _occ_blocked;
	float _occ_ai_ms;
	u32 _occ_ai_calls;
	u32 _emitters_3d;
};

class XRSOUND_API CSound_stats_ext
{
public:
	struct SItem
	{
		shared_str name;
		CSound_params params;
		float volume;
		esound_type type;
		int game_type;
		CObject* game_object;

		struct
		{
			u32 _3D :1;
			u32 _rendered :1;
		};
	};

	DEFINE_VECTOR(SItem, item_vec, item_vec_it);
	item_vec items;
public:
	void clear() { items.clear(); }
	void append(const SItem& itm) { items.push_back(itm); }
};

/// definition (Sound Callback)
typedef void __stdcall sound_event(ref_sound_data_ptr S, float range);

/// Cyclic gunfire in the engine (doc 08): AI-hearing event raised by the native voice. The voice has no
/// ref_sound/emitter behind it, so it cannot go through sound_event; the game side turns this into the same
/// feel_sound_new() notification a vanilla weapon shot would produce. max_ai_dist comes from the AI-reaction
/// distance in the sample's OGG comment, volume is the gain the shot is heard at.
typedef void __stdcall sound_event_raw(CObject* who, int g_type, const Fvector& pos, float max_ai_dist, float volume);

/// Cyclic gunfire in the engine (doc 08): one event handed by the weapon code to the native voice. The weapon
/// (xrGame) resolves everything that depends on weapon state / .ltx (which samples for suppressed / subsonic /
/// ADS / indoor, timing, fades) and passes the result here, so xrSound never reads the weapon configs and no
/// script is involved per shot.
enum ECyclicEvent
{
	cyc_begin = 0, // first shot of a burst
	cyc_shot,      // any following shot
	cyc_release,   // the weapon stopped firing (trigger released, reload, hide...)
	cyc_abort,     // stop everything now, no end sample (death, weapon dropped / destroyed)
	cyc_jam,       // jam: cut the loop now and play the end sample
};

struct SCyclicEvent
{
	ECyclicEvent type = cyc_shot;
	CObject* owner = nullptr; // the actor (AI-hearing events)
	int ai_type = 0;          // the weapon's own AI sound type (SOUND_TYPE_WEAPON_SHOOTING | ...)
	int shot_no = 0;          // 1-based shot number inside the burst
	bool last_shot = false;   // the weapon already knows that no further shot will follow
	float real_rpm = 600.f;   // actual cadence of the weapon right now (upgrades included)

	shared_str start_name;                      // sample played on shot 1 (variant already picked)
	shared_str loop_name;                       // block loop, "" if the weapon has none (semi-auto)
	float loop_sample_rpm = 600.f;              // rpm the loop was mixed at -> block length = 60 / rpm
	int loop_shots = 0;                         // number of blocks in the loop file
	shared_str trans_name;                      // optional indoor<->outdoor transition loop, "" if none
	float trans_sample_rpm = 600.f;
	int trans_shots = 0;
	shared_str end_name;                        // tail played when firing stops (variant already picked)

	float loop_delay_s = 0.f;                   // snd_shoot_loop_delay
	float end_margin_s = 0.f;                   // snd_shoot_end_cycle_margin
	bool start_cutoff_on_loop = true;           // snd_shoot_start_cutoff_*
	bool start_cutoff_fade_enable = true;
	bool start_cutoff_soft = false;
	float start_cutoff_fade_s = 0.03f;
	int start_cutoff_fade_curve = 1;            // 0 linear, 1 equal-power
	bool loop_fadeout_enable = false;           // snd_shoot_loop_fadeout_*
	float loop_fadeout_s = 0.06f;
	int loop_fadeout_curve = 1;
	bool end_fadein_enable = false;             // snd_shoot_end_fadein_*
	float end_fadein_s = 0.03f;
	int end_fadein_curve = 1;
};

/// definition (Sound Manager Interface)
class XRSOUND_API CSound_manager_interface
{
	virtual void _initialize(int stage) = 0;
	virtual void _clear() = 0;

protected:
	friend class ref_sound_data;
	virtual void _create_data(ref_sound_data& S, LPCSTR fName, esound_type sound_type, int game_type) = 0;
	virtual void _destroy_data(ref_sound_data& S) = 0;
public:
	virtual ~CSound_manager_interface()
	{
	}

	static void _create(int stage);
	static void _destroy();

	virtual void _restart() = 0;
	virtual BOOL i_locked() = 0;
	virtual BOOL is_ready() = 0;
	virtual void refresh_devices() = 0;
	virtual void default_device_changed() = 0;
	virtual void switch_device(LPCSTR device_name) = 0;

	virtual void create(ref_sound& S, LPCSTR fName, esound_type sound_type, int game_type) = 0;
	virtual void attach_tail(ref_sound& S, LPCSTR fName) = 0;
	virtual void clone(ref_sound& S, const ref_sound& from, esound_type sound_type, int game_type) = 0;
	virtual void destroy(ref_sound& S) = 0;
	virtual void stop_emitters() = 0;
	virtual int pause_emitters(bool val) = 0;

	virtual void play(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f) = 0;
	virtual void play_at_pos(ref_sound& S, CObject* O, const Fvector& pos, u32 flags = 0, float delay = 0.f) = 0;
	virtual void play_no_feedback(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f, Fvector* pos = 0,
	                              float* vol = 0, float* freq = 0, Fvector2* range = 0) = 0;

	virtual void set_master_volume(float f = 1.f) = 0;
	virtual void set_geometry_env(IReader* I) = 0;
	virtual void set_geometry_som(IReader* I) = 0;
	virtual void set_geometry_occ(CDB::MODEL* M) = 0;
	virtual void set_handler(sound_event* E) = 0;

	virtual void update(const Fvector& P, const Fvector& D, const Fvector& N) = 0;
	virtual void statistic(CSound_stats* s0, CSound_stats_ext* s1) = 0;

	virtual float get_occlusion_to(const Fvector& hear_pt, const Fvector& snd_pt, float dispersion = 0.2f) = 0;
	virtual float get_occlusion(Fvector& P, float R, Fvector* occ) = 0;

	// Phase 1/2 occlusion rework (29/09), "snd_occlusion_mode 1". See docs/ENGINE_CHANGES_SOUND_OCCLUSION.md.
	// Per-material acoustic losses, keyed by the same material ID CDB::TRI/RESULT already carry -- built
	// once per level load from GameMtlLib + sound_occlusion.ltx (xrEngine side, which already depends on
	// GMLib) and handed to xrSound as a flat table, so xrSound itself never needs to depend on GMLib.
	virtual void set_occlusion_materials(const SSoundOcclusionMaterial* table, u32 count) = 0;
	virtual void set_occlusion_limits(float max_loss_db, float max_hf_loss_db, float max_thickness_m,
		float max_loss_db_indoor, float max_hf_loss_db_indoor) = 0;

	// Actor-fire priority ducking (30/09). Called once from CActor::on_weapon_shot_start() -- the sound
	// side records the timestamp on its own clock and derives the duck envelope from elapsed time, so no
	// cross-module clock synchronization is needed. No-op at snd_duck_mode 0.
	virtual void on_actor_weapon_shot() = 0;

	// Cyclic gunfire in the engine (doc 08). True only when snd_cyclic_native is on AND the voice backend is
	// usable on this device. Queried by the weapon code before it hands a shot to the native voice and by the
	// scripts (cyclic_voice.ready()) to know whether the scripted system must stand down.
	virtual bool cyclic_native_ready() = 0;
	// Called by the weapon code for every event of a burst of a custom_loop_sound weapon (see SCyclicEvent).
	virtual void cyclic_event(const SCyclicEvent& e) = 0;
	virtual void set_handler_raw(sound_event_raw* E) = 0;

	virtual void object_relcase(CObject* obj) = 0;
	virtual const Fvector& listener_position() = 0;
#ifdef __BORLANDC__
	virtual SoundEnvironment_LIB*	get_env_library			()																						= 0;
	virtual void					refresh_env_library		()																						= 0;
	virtual void					set_user_env			(CSound_environment* E)																	= 0;
	virtual void					refresh_sources			()																						= 0;
    virtual void					set_environment			(u32 id, CSound_environment** dst_env)													= 0;
    virtual void					set_environment_size	(CSound_environment* src_env, CSound_environment** dst_env)								= 0;
#endif
};

extern XRSOUND_API CSound_manager_interface* Sound;

/// ********* Sound ********* (utils, accessors, helpers)
IC ref_sound_data::ref_sound_data()
{
	handle = 0;
	feedback = 0;
	g_type = 0;
	g_object = 0;
	s_type = st_Effect;
}

IC ref_sound_data::ref_sound_data(LPCSTR fName, esound_type sound_type, int game_type)
{
	::Sound->_create_data(*this, fName, sound_type, game_type);
}

IC ref_sound_data::~ref_sound_data() { ::Sound->_destroy_data(*this); }

IC void ref_sound::create(LPCSTR name, esound_type sound_type, int game_type)
{
	VERIFY(!::Sound->i_locked());
	::Sound->create(*this, name, sound_type, game_type);
}

IC void ref_sound::attach_tail(LPCSTR name)
{
	VERIFY(!::Sound->i_locked());
	::Sound->attach_tail(*this, name);
}

IC void ref_sound::clone(const ref_sound& from, esound_type sound_type, int game_type)
{
	VERIFY(!::Sound->i_locked());
	::Sound->clone(*this, from, sound_type, game_type);
}

IC void ref_sound::destroy()
{
	VERIFY(!::Sound->i_locked());
	::Sound->destroy(*this);
}

IC void ref_sound::play(CObject* O, u32 flags, float d)
{
	VERIFY(!::Sound->i_locked());
	::Sound->play(*this, O, flags, d);
}

IC void ref_sound::play_at_pos(CObject* O, const Fvector& pos, u32 flags, float d)
{
	VERIFY(!::Sound->i_locked());
	::Sound->play_at_pos(*this, O, pos, flags, d);
}

IC void ref_sound::play_no_feedback(CObject* O, u32 flags, float d, Fvector* pos, float* vol, float* freq,
                                    Fvector2* range)
{
	VERIFY(!::Sound->i_locked());
	::Sound->play_no_feedback(*this, O, flags, d, pos, vol, freq, range);
}

IC void ref_sound::set_position(const Fvector& pos)
{
	VERIFY(!::Sound->i_locked());
	VERIFY(_feedback());
	_feedback()->set_position(pos);
}

IC void ref_sound::set_frequency(float freq)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_frequency(freq);
}

IC void ref_sound::set_range(float min, float max)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_range(min, max);
}

IC void ref_sound::set_volume(float vol)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_volume(vol);
}

IC void ref_sound::set_priority(float p)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_priority(p);
}

IC void ref_sound::stop()
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->stop(FALSE);
}

IC void ref_sound::stop_deffered()
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->stop(TRUE);
}

// duration_s <= 0 restores default engine behaviour (0.1s, linear) for this call.
// curve: 0 = linear, 1 = equal-power (cosine/sine) -- smoother-sounding for crossfades.
IC void ref_sound::set_fade_out(float duration_s, int curve)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_fade_out(duration_s, curve);
}

// duration_s <= 0 disables the fade-in (sound starts at full volume immediately,
// matching default engine behaviour for every sound that doesn't call this).
IC void ref_sound::set_fade_in(float duration_s, int curve)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_fade_in(duration_s, curve);
}

IC const CSound_params* ref_sound::get_params()
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) return _feedback()->get_params();
	else return NULL;
}

IC void ref_sound::set_params(CSound_params* p)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback())
	{
		_feedback()->set_position(p->position);
		_feedback()->set_frequency(p->freq);
		_feedback()->set_range(p->min_distance, p->max_distance);
		_feedback()->set_volume(p->volume);
	}
}
#endif
