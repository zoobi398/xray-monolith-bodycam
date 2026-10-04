#ifndef SoundRender_EmitterH
#define SoundRender_EmitterH
#pragma once

#include "soundrender.h"
#include "SoundRender_Core.h"
#include "soundrender_environment.h"

class CSoundRender_Emitter : public CSound_emitter
{
	float starting_delay;
public:
	enum State
	{
		stStopped = 0,

		stStartingDelayed,
		stStartingLoopedDelayed,

		stStarting,
		stStartingLooped,

		stPlaying,
		stPlayingLooped,

		stSimulating,
		stSimulatingLooped,

		stFORCEDWORD = u32(-1)
	};

private:
	bool need_preplay_update;

public:
#ifdef DEBUG
	u32							dbg_ID;
#endif

	CSoundRender_Target* target;
	IC CSoundRender_Source* source() { return (CSoundRender_Source*)owner_data->handle; };
	ref_sound_data_ptr owner_data;

	u32 get_bytes_total() const;
	float get_length_sec() const;

	float priority_scale;
	float smooth_volume;
	float occluder_volume; // USER
	float fade_volume;

	// Configurable fade-out (on stop_deffered) / fade-in (on play) for 2D emitters.
	// Defaults reproduce the original hardcoded behaviour (0.1s, linear, fade-in disabled)
	// for every sound that never calls set_fade_out/set_fade_in.
	float fade_out_duration_s;
	float fade_in_duration_s;
	int   fade_out_curve; // 0 = linear, 1 = equal-power
	int   fade_in_curve;  // 0 = linear, 1 = equal-power
	float fade_out_elapsed;
	float fade_in_elapsed;

	Fvector occluder [3];

	// Phase 1/2 occlusion rework (29-30/09, "snd_occlusion_mode 1"). occluder_volume (existing field,
	// above) is repurposed at mode 1 to mean the DIRECT path's smoothed broadband gain (feeds
	// m_direct_filter's AL_LOWPASS_GAIN, NOT AL_GAIN -- see update_culling/SoundRender_TargetA).
	// occluder_gain_wet is the separate, smoothed reverb-SEND broadband gain (m_send_filter's
	// AL_LOWPASS_GAIN). occluder_gain_hf mirrors both filters' AL_LOWPASS_GAINHF. occ_target_* are the
	// last full get_occlusion_ex() result; update_culling() smoothly follows them at ~4/s instead of
	// recomputing every frame. occ_profile is resolved once in start() from the sound's SOUND_TYPE_*
	// (impulse/loop/light -- see get_occlusion_ex's profile parameter).
	float occluder_gain_hf;
	float occluder_gain_wet;
	float occ_target_gain;
	float occ_target_hf;
	float occ_target_wet_gain;
	float occ_next_update; // SoundRender->fTimer_Value-based, when the next full re-evaluation is due
	u8 occ_profile;
	// Whether THIS voice is a looped emission (resolved once in start() from its own _loop parameter --
	// deliberately NOT derived from occ_profile, since a weapon's own sustained fire-loop sound is still
	// classified occ_profile==1 "impulse"). Gates whether update_culling() keeps re-evaluating occlusion
	// for the life of this voice (loop) or freezes it at the single stStarting/stStartingLooped result
	// (one-shot) -- see the "Update (01/10)" note in update_culling.
	bool occ_is_loop;
	// One-shot voices only: true once occ_target_* hold the occlusion of the moment the shot happened
	// (taken on the first update with a valid position, possibly while still in stStartingDelayed).
	bool occ_snapshot_valid;

	// Actor-fire priority ducking (30/09, "snd_duck_mode 1"). A separate multiplier, NOT folded into
	// occluder_volume (which is occlusion's own smoothed state and must not be contaminated by an
	// unrelated signal). Applied directly to AL_GAIN via smooth_volume (see update_culling) -- unlike
	// occlusion, ducking is meant to reduce both the direct signal AND the reverb send equally (auditory
	// masking is a property of the listener, not of the sound's propagation path), and AL_GAIN already
	// scales both proportionally, so no separate filter routing is needed here.
	float duck_gain;

	State m_current_state;
	u32 m_stream_cursor;
	u32 m_cur_handle_cursor;
	CSound_params p_source;

	int iPaused;
	BOOL bMoved;
	BOOL b2D;
	bool bIntro;
	BOOL bStopping;
	BOOL bRewind;
	float fTimeStarted; // time of "Start"
	float fTimeToStop; // time to "Stop"
	float fTimeToPropagade;

	u32 marker;
	void i_stop();

	void set_cursor(u32 p);
	u32 get_cursor(bool b_absolute) const;
	void move_cursor(int offset);

public:
	void Event_Propagade();
	void Event_ReleaseOwner();
	BOOL isPlaying(void) { return m_current_state != stStopped; }

	virtual BOOL is_2D() { return b2D; }
	virtual void switch_to_2D();
	virtual void switch_to_Intro() override;
	virtual void switch_to_3D();
	virtual void set_position(const Fvector& pos);

	virtual void set_frequency(float scale)
	{
		VERIFY(_valid(scale));
		p_source.freq = scale;

		// demonized: if the sound is short, apply pitch variation, so that stuff like music and most of speech won't be randomized
		if (get_length_sec() < 10)
			p_source.freq *= (1.f + p_source.pitch_variation);

		if (fTimeToStop != 0.f)
			fTimeToStop = SoundRender->fTimer_Value + ((get_length_sec() - (SoundRender->fTimer_Value - fTimeStarted)) / (scale * psSpeedOfSound));
	}

	virtual void set_range(float min, float max)
	{
		VERIFY(_valid(min)&&_valid(max));
		p_source.min_distance = min;
		p_source.max_distance = max;
	}

	virtual void set_volume(float vol)
	{
		if (!_valid(vol)) vol = 0.0f;
		p_source.volume = vol;
	}

	virtual void set_priority(float p) { priority_scale = p; }
	virtual const CSound_params* get_params() { return &p_source; }

	void fill_block(void* ptr, u32 size);
	void fill_data(u8* ptr, u32 offset, u32 size);

	float priority();
	void start(ref_sound* _owner, BOOL _loop, float delay);
	void cancel(); // manager forces out of rendering
	void update(float dt);
	BOOL update_culling(float dt);
	void update_environment(float dt);
	void rewind();
	virtual void stop(BOOL bDeffered);
	virtual void set_fade_out(float duration_s, int curve);
	virtual void set_fade_in(float duration_s, int curve);
	void pause(BOOL bVal, int id);

	virtual u32 play_time();

	CSoundRender_Emitter();
	~CSoundRender_Emitter();
};
#endif
