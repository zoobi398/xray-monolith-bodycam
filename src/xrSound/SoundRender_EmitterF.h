#pragma once

#include "Sound.h"

namespace FMOD
{
	class Sound;
	class Channel;
}

// FMOD Core-backed implementation of CSound_emitter, used only for the 2D
// (non-positional) playback path - see CSoundRender_Core::play() routing.
// Deliberately narrower than CSoundRender_Emitter (the OpenAL emitter): no
// position/range/occlusion/environment, since none of that applies to 2D
// sound. Fade in/out mirrors the engine's own elapsed-time fade math
// (SoundRender_Emitter_FSM.cpp) for behavioral parity with the OpenAL path;
// switching to FMOD's native fade points is a later polish step, not v1.
//
// Lifetime: created per-play (no pooling, unlike the OpenAL emitter) and
// self-removes via the static registry once stopped/finished - see
// update_all()'s cleanup pass.
class CSoundRender_EmitterF : public CSound_emitter
{
	FMOD::Sound* m_sound;
	FMOD::Channel* m_channel;
	CSound_params m_params;
	ref_sound_data_ptr owner_data;

	BOOL m_stopping;
	bool m_finished;

	float m_fade_out_duration_s;
	float m_fade_in_duration_s;
	int m_fade_out_curve;
	int m_fade_in_curve;
	float m_fade_out_elapsed;
	float m_fade_in_elapsed;
	float m_fade_volume;

	static xr_vector<CSoundRender_EmitterF*> s_all;

	void apply_volume();

public:
	CSoundRender_EmitterF();
	virtual ~CSoundRender_EmitterF();

	void start(ref_sound* owner, BOOL loop, float delay);
	void update(float dt);
	void i_stop();

	virtual BOOL is_2D() override { return TRUE; }
	virtual void switch_to_2D() override {}
	virtual void switch_to_Intro() override {}
	virtual void switch_to_3D() override {}
	virtual void set_position(const Fvector& pos) override {}
	virtual void set_frequency(float freq) override;
	virtual void set_range(float min, float max) override {}
	virtual void set_volume(float vol) override;
	virtual void set_priority(float p) override {}
	virtual void stop(BOOL bDeffered) override;
	virtual void set_fade_out(float duration_s, int curve) override;
	virtual void set_fade_in(float duration_s, int curve) override;
	virtual const CSound_params* get_params() override { return &m_params; }
	virtual u32 play_time() override;
	virtual void rewind() override;
	virtual bool is_fmod_backed() const override { return true; }

	static void update_all(float dt);
	static void destroy_all();
};
