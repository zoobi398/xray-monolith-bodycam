#pragma once

#include "SoundRender.h"
#include "SoundRender_Environment.h"
#include "SoundRender_Cache.h"

class CNotificationClient;

class CSoundRender_Core : public CSound_manager_interface
{
	volatile BOOL bLocked;
protected:
	virtual void _create_data(ref_sound_data& S, LPCSTR fName, esound_type sound_type, int game_type);
	virtual void _destroy_data(ref_sound_data& S);
	CNotificationClient* pSysNotification = nullptr;
public:
	volatile BOOL bPendingDefaultDeviceSwitch = FALSE;
	volatile BOOL bPendingDeviceListRefresh = FALSE;
protected:
	BOOL bListenerMoved;

	CSoundRender_Environment e_current;
	CSoundRender_Environment e_identity;
	CSoundRender_Environment* e_target_ptr;

public:
	typedef std::pair<ref_sound_data_ptr, float> event;
	xr_vector<event> s_events;
public:
	BOOL bPresent;
	BOOL bUserEnvironment;
	BOOL bReady;
	bool m_is_supported; // Boolean variable to indicate presence of EFX Extension

	CTimer Timer;
	float fTimer_Value;
	float fTimer_Delta;
	sound_event* Handler;
protected:
	// Collider
#ifndef _EDITOR
	CDB::COLLIDER geom_DB;
#endif
	CDB::MODEL* geom_SOM;
	CDB::MODEL* geom_MODEL;
	CDB::MODEL* geom_ENV;

	// Containers
	xr_unordered_map<xr_string, CSoundRender_Source*> s_sources;
	xr_vector<CSoundRender_Emitter*> s_emitters;
	u32 s_emitters_u; // emitter update marker
	xr_vector<CSoundRender_Target*> s_targets;
	xr_vector<CSoundRender_Target*> s_targets_defer;
	u32 s_targets_pu; // parameters update
	SoundEnvironment_LIB* s_environment;
	CSoundRender_Environment s_user_environment;

	int m_iPauseCounter;
public:
	// Cache
	CSoundRender_Cache cache;
	u32 cache_bytes_per_line;

public:
	// Phase 0 occlusion instrumentation (29/09, "snd_occlusion_stats"). Public rather than
	// friend/accessor-gated to match this class's existing style (s_emitters, Timer, etc. are already
	// public) -- CSoundRender_Emitter::update_culling (SoundRender_Emitter_FSM.cpp) increments
	// emitters_3d directly via the global SoundRender pointer. Purely additive counters: nothing here
	// changes what get_occlusion/get_occlusion_to/update_culling actually compute.
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

	// Phase 1/2 occlusion rework (29/09, "snd_occlusion_mode 1"). Material table indexed directly by
	// CDB::TRI/RESULT's material ID (bounds-checked on every lookup -- the 14-bit field can reference an
	// ID this table doesn't know about, e.g. stale compiled geometry). Built once per level load by
	// IGame_Level.cpp via set_occlusion_materials(); empty (size 0) until then, in which case every hit
	// falls back to m_occ_default_material.
	xr_vector<SSoundOcclusionMaterial> m_occ_materials;
	SSoundOcclusionMaterial m_occ_default_material;
	float m_occ_max_loss_db = 28.f;
	float m_occ_max_hf_loss_db = 36.f;
	float m_occ_max_thickness_m = 12.f;
	float m_occ_max_loss_db_indoor = 28.f;    // same as outdoor unless sound_occlusion.ltx says otherwise
	float m_occ_max_hf_loss_db_indoor = 36.f;

	// Per-frame full-evaluation budget (snd_occlusion_budget) -- reset in update(), consumed by
	// get_occlusion_ex callers via occ_budget_take(). Shot starts (stStarting/stStartingLooped) bypass
	// the budget entirely (a shot must always be evaluated); only the cadence-gated update_culling path
	// is budget-limited.
	u32 m_occ_budget_used = 0;
	IC bool occ_budget_take()
	{
		if (psSoundOcclusionBudget <= 0 || m_occ_budget_used < (u32)psSoundOcclusionBudget)
		{
			m_occ_budget_used++;
			return true;
		}
		return false;
	}

	struct SSoundOcclusionResult
	{
		float gain = 1.f;     // direct/dry path broadband gain, 0-1
		float gain_hf = 1.f;  // direct path AL_LOWPASS_GAINHF, 0-1 (1 = no filtering)
		float wet_gain = 1.f; // reverb SEND path broadband gain -- deliberately NOT the same as 'gain'
		                      // (see snd_occlusion_wet_sensitivity): a real obstacle chokes the direct
		                      // sound far more than it chokes the room's own reflected energy, which
		                      // keeps arriving via paths this system never traces.
		bool blocked = false;
		bool diffracted = false; // true if an over/around path won over the direct one
	};

	virtual void set_occlusion_materials(const SSoundOcclusionMaterial* table, u32 count) override;
	virtual void set_occlusion_limits(float max_loss_db, float max_hf_loss_db, float max_thickness_m,
		float max_loss_db_indoor, float max_hf_loss_db_indoor) override;

	// Actor-fire priority ducking (30/09). fTimer_Value timestamp of the actor's last weapon shot -- far
	// in the past initially so nothing is "ducked" before the first shot ever fires. Read directly by
	// CSoundRender_Emitter::update_culling() via the global SoundRender pointer, same access pattern as
	// fTimer_Value itself.
	float m_actor_last_shot_time = -1000.f;
	virtual void on_actor_weapon_shot() override { m_actor_last_shot_time = fTimer_Value; }

	// Thin passthroughs to the AL_EXT_EFX filter object functions, which are per-device extension
	// function pointers private to CSoundRender_CoreA (loaded via LOAD_PROC, not statically linked).
	// Base implementation is a no-op/unsupported so this class stays usable without an A-backend;
	// CSoundRender_CoreA overrides all three to call the real functions. SoundRender_TargetA.cpp calls
	// these through the generic SoundRender pointer instead of downcasting. u32 here instead of ALuint
	// so this AL-agnostic base header doesn't need to pull in <AL/al.h> -- both are unsigned int.
	virtual u32 occ_gen_filter() { return 0; }
	virtual void occ_delete_filter(u32 id) {}
	virtual void occ_set_filter_lowpass(u32 id, float gain, float gain_hf) {}

	// profile: 0 = light (1 sample), 1 = impulse (5 samples + diffraction), 2 = loop (3 samples).
	// debug_name/is_weapon_shot only drive the optional snd_occlusion_debug log line -- purely cosmetic,
	// never change the computed result.
	SSoundOcclusionResult get_occlusion_ex(const Fvector& src, u32 profile, LPCSTR debug_name = nullptr,
		bool is_weapon_shot = false);

private:
	// One "all hits, sorted, material-priced" ray between two points. Every surface crossed adds its
	// class's loss_db once; loss_db_per_m only applies when the NEXT hit shares the same material (a
	// real measured entry+exit pair) -- never guessed for a lone/unpaired hit. Increments 'rays'.
	void occ_trace_losses(const Fvector& from, const Fvector& to, float& out_loss_db, float& out_hf_db, u32& rays,
		float* out_raw_loss_db = nullptr);

	// 0 = outdoor values, 1 = indoor values (see Sound.h). 0 whenever snd_occlusion_indoor_mode is off.
	IC float occ_indoor_f() const
	{
		return psSoundOcclusionIndoorMode ? _max(0.f, _min(psSoundOcclusionIndoorFactor, 1.f)) : 0.f;
	}

	// Phase 2 diffraction (29/09): tests candidate over/around paths and returns the best one found (or
	// blocked=true, gain=0 if none is clear). Caller combines with the direct-path result via
	// gain=max(direct,diffracted).
	SSoundOcclusionResult occ_try_diffraction(const Fvector& L, const Fvector& S, float direct_dist);

public:
	CSoundRender_Core();
	virtual ~CSoundRender_Core();

	// General
	virtual void _initialize(int stage) =0;
	virtual void _clear() =0;
	virtual void _restart();
	virtual void switch_device(LPCSTR device_name) {}
	virtual void refresh_devices() {}

	// Sound interface
	void verify_refsound(ref_sound& S);
	virtual void create(ref_sound& S, LPCSTR fName, esound_type sound_type, int game_type);
	virtual void attach_tail(ref_sound& S, LPCSTR fName);

	virtual void clone(ref_sound& S, const ref_sound& from, esound_type sound_type, int game_type);
	virtual void destroy(ref_sound& S);
	virtual void stop_emitters();
	virtual void restart_emitters();
	virtual int pause_emitters(bool val);

	virtual void play(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f);
	virtual void play_at_pos(ref_sound& S, CObject* O, const Fvector& pos, u32 flags = 0, float delay = 0.f);
	virtual void play_no_feedback(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f, Fvector* pos = 0,
	                              float* vol = 0, float* freq = 0, Fvector2* range = 0);
	virtual void set_master_volume(float f) =0;
	virtual void set_geometry_env(IReader* I);
	virtual void set_geometry_som(IReader* I);
	virtual void set_geometry_occ(CDB::MODEL* M);
	virtual void set_handler(sound_event* E);

	virtual void update(const Fvector& P, const Fvector& D, const Fvector& N);
	virtual void update_events();
	virtual void statistic(CSound_stats* dest, CSound_stats_ext* ext);

	// listener
	virtual void update_listener(const Fvector& P, const Fvector& D, const Fvector& N, float dt)=0;
	
	//  EFX listener
	virtual void set_listener(const CSoundRender_Environment& env)=0;
	virtual void get_listener(CSoundRender_Environment& env)=0;
	virtual void commit()=0;

#ifdef _EDITOR
	virtual SoundEnvironment_LIB*		get_env_library			()																{ return s_environment; }
	virtual void						refresh_env_library		();
	virtual void						set_user_env			(CSound_environment* E);
	virtual void						refresh_sources			();
    virtual void						set_environment			(u32 id, CSound_environment** dst_env);
    virtual void						set_environment_size	(CSound_environment* src_env, CSound_environment** dst_env);
#endif
public:
	CSoundRender_Source* i_create_source(LPCSTR name);
	void i_destroy_source(CSoundRender_Source* S);
	CSoundRender_Emitter* i_play(ref_sound* S, BOOL _loop, float delay);
	void i_start(CSoundRender_Emitter* E);
	void i_stop(CSoundRender_Emitter* E);
	void i_rewind(CSoundRender_Emitter* E);
	BOOL i_allow_play(CSoundRender_Emitter* E);
	virtual BOOL i_locked() { return bLocked; }
	virtual BOOL is_ready() { return bReady; }

	virtual void object_relcase(CObject* obj);
	void i_create_all_sources();

	virtual float get_occlusion_to(const Fvector& hear_pt, const Fvector& snd_pt, float dispersion = 0.2f);
	float get_occlusion(Fvector& P, float R, Fvector* occ) override;
	CSoundRender_Environment* get_environment(const Fvector& P);

	void env_load();
	void env_unload();
	void env_apply();
};

extern CSoundRender_Core* SoundRender;
