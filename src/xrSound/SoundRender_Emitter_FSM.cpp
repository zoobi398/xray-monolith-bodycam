#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_Emitter.h"
#include "SoundRender_Core.h"
#include "SoundRender_Source.h"

//#define MEASURE_PROCESSING_TIME

#ifdef MEASURE_PROCESSING_TIME
	CTimer Timer;
#endif

XRSOUND_API extern float psSoundCull;

inline u32 calc_cursor(const float& fTimeStarted, float& fTime, const float& fTimeTotal, const WAVEFORMATEX& wfx)
{
	if (fTime < fTimeStarted)
		fTime = fTimeStarted; // Андрюха посоветовал, ассерт что ниже вылетел из за паузы как то хитро
	R_ASSERT((fTime-fTimeStarted)>=0.0f);
	while ((fTime - fTimeStarted) > fTimeTotal) //looped
	{
		fTime -= fTimeTotal;
	}
	u32 curr_sample_num = iFloor((fTime - fTimeStarted) * wfx.nSamplesPerSec);
	return curr_sample_num * (wfx.wBitsPerSample / 8) * wfx.nChannels;
}

void CSoundRender_Emitter::update(float dt)
{
	float fTime = SoundRender->fTimer_Value;
	float fDeltaTime = SoundRender->fTimer_Delta;

	VERIFY2(!!(owner_data) || (!(owner_data)&&(m_current_state==stStopped)), "owner");
	VERIFY2(owner_data?*(int*)(&owner_data->feedback):1, "owner");

#ifdef MEASURE_PROCESSING_TIME
	float time1 = Timer.GetElapsed_ms_f();
#endif

	if (bRewind)
	{
		if (target) SoundRender->i_rewind(this);
		bRewind = FALSE;
	}

	if (m_current_state != stStopped && !_valid(p_source.position))
	{
		m_current_state = stStopped;
	}

	// demonized: add preplay update, calculate delay based on distance and speed of sound
	// Adaptation for user experience
	// For distances < 30m (configurable via cvar), no delay
	// Then gradually ramp up to delay calculated by math
	// Starting at 90m, full delay kicks in
	// Don't apply delay to already delayed sounds, they are most likely already handled in respective parts of code and scripts
	if (need_preplay_update && m_current_state < stPlaying && starting_delay == 0.f)
	{
		if (_valid(p_source.position) && !p_source.position.similar(Fvector().set(0.f, 0.f, 0.f)) && owner_data && source() && source()->channels_num() == 1)
		{
			//Smooth Ramp Using Hermite(Smootherstep)
			static auto CalculateSmoothSoundDelay = [](float distance, float speedOfSound, float rampStart, float rampRange)
			{
				if (distance <= rampStart)
					return 0.0f;

				float delay = distance / speedOfSound;

				// Ramp factor from 0 to 1 between rampStart and rampStart + rampRange
				float t = std::clamp((distance - rampStart) / rampRange, 0.0f, 1.0f);

				// Smootherstep for smooth transition
				float smoothT = t * t * t * (t * (t * 6 - 15) + 10);

				return delay * smoothT;
			};

			float speedOfSound = 343.f;
			float oldDelay = starting_delay;
			auto oldState = m_current_state;
			auto delay = CalculateSmoothSoundDelay(p_source.position.distance_to(SoundRender->listener_position()), speedOfSound, soundSmoothingParams::distanceBasedDelayMinDistance, 60);

			// clamp delay in case of strange result
			delay = std::clamp(delay, 0.f, 3.5f);

			// apply cvar power
			delay *= soundSmoothingParams::distanceBasedDelayPower;
			if (delay > 0.f)
			{
				if (m_current_state == stStarting || m_current_state == stStartingLooped)
				{
					starting_delay = delay;
					m_current_state = m_current_state == stStarting ? stStartingDelayed : stStartingLoopedDelayed;
				}
				else
				{
					starting_delay += delay;
				}
				need_preplay_update = false;
				/*Msg("CSoundRender_Emitter::update, file %s, state %s, need_preplay_update, distance %.2f, old delay %.2f, delay %.2f",
					owner_data && source() && source()->file_name() ? source()->file_name() : "null",
					magic_enum::enum_name(oldState).data(),
					p_source.position.distance_to(SoundRender->listener_position()),
					oldDelay,
					starting_delay
				);*/
			}
		}
	}
	else
	{
		need_preplay_update = false;
	}

	switch (m_current_state)
	{
	case stStopped:
		break;
	case stStartingDelayed:
		if (iPaused) break;
		starting_delay -= dt;
		if (starting_delay <= 0)
			m_current_state = stStarting;
		break;
	case stStarting:
		if (iPaused) break;
		fTimeStarted = fTime;
		fTimeToStop = fTime + (get_length_sec() / psSpeedOfSound); 
		fTimeToPropagade = fTime;
		fade_volume = 1.f;
		if (!b2D && psSoundOcclusionMode == 1)
		{
			// Phase 1/2 (29/09): a shot's own defining sound -- always fully evaluated (budget doesn't
			// apply here), assigned directly with no smoothing so the very first frame already carries
			// the real result. update_culling() right below picks up from here with its normal
			// cadence-gated re-evaluation; occ_next_update is set so it doesn't immediately redo this
			// same work on this same frame.
			CSoundRender_Core::SSoundOcclusionResult r = SoundRender->get_occlusion_ex(p_source.position,
				occ_profile, source() ? source()->file_name() : nullptr, occ_profile == 1);
			occluder_volume = r.gain;       // repurposed at mode 1: direct-filter broadband gain, not AL_GAIN
			occluder_gain_hf = r.gain_hf;
			occluder_gain_wet = r.wet_gain; // reverb-send broadband gain, independently smoothed
			occ_target_gain = r.gain;
			occ_target_hf = r.gain_hf;
			occ_target_wet_gain = r.wet_gain;
			occ_next_update = SoundRender->fTimer_Value + _max(psSoundOcclusionUpdateMs, 0) / 1000.f;
		}
		else
			occluder_volume = SoundRender->get_occlusion(p_source.position, .2f, occluder);
		// Mode 1, 3D: occlusion no longer multiplies AL_GAIN at all (see design note in
		// update_culling below) -- it now lives entirely in the two filters' own gains instead, so the
		// reverb send isn't forced through the same attenuation as the direct signal.
		smooth_volume = p_source.base_volume * p_source.volume * (owner_data->s_type == st_Effect
			                                                          ? psSoundVEffects * psSoundVFactor
			                                                          : psSoundVMusic * psSoundVMusicFactor) *
			(b2D ? 1.f : (psSoundOcclusionMode == 1 ? 1.f : occluder_volume));
		if (update_culling(dt))
		{
			m_current_state = stPlaying;
			set_cursor(0);
			SoundRender->i_start(this);
		}
		else
			m_current_state = stSimulating;
		break;
	case stStartingLoopedDelayed:
		if (iPaused) break;
		starting_delay -= dt;
		if (starting_delay <= 0)
			m_current_state = stStartingLooped;
		break;
	case stStartingLooped:
		if (iPaused) break;
		fTimeStarted = fTime;
		fTimeToStop = 0xffffffff;
		fTimeToPropagade = fTime;
		fade_volume = 1.f;
		if (!b2D && psSoundOcclusionMode == 1)
		{
			// Phase 1/2 (29/09): see the identical block in the stStarting case above.
			CSoundRender_Core::SSoundOcclusionResult r = SoundRender->get_occlusion_ex(p_source.position,
				occ_profile, source() ? source()->file_name() : nullptr, occ_profile == 1);
			occluder_volume = r.gain;
			occluder_gain_hf = r.gain_hf;
			occluder_gain_wet = r.wet_gain;
			occ_target_gain = r.gain;
			occ_target_hf = r.gain_hf;
			occ_target_wet_gain = r.wet_gain;
			occ_next_update = SoundRender->fTimer_Value + _max(psSoundOcclusionUpdateMs, 0) / 1000.f;
		}
		else
			occluder_volume = SoundRender->get_occlusion(p_source.position, .2f, occluder);
		smooth_volume = p_source.base_volume * p_source.volume * (owner_data->s_type == st_Effect
			                                                          ? psSoundVEffects * psSoundVFactor
			                                                          : psSoundVMusic * psSoundVMusicFactor) *
			(b2D ? 1.f : (psSoundOcclusionMode == 1 ? 1.f : occluder_volume));
		if (update_culling(dt))
		{
			m_current_state = stPlayingLooped;
			set_cursor(0);
			SoundRender->i_start(this);
		}
		else
			m_current_state = stSimulatingLooped;
		break;
	case stPlaying:
		if (iPaused)
		{
			if (target)
			{
				SoundRender->i_stop(this);
				m_current_state = stSimulating;
			}
			fTimeStarted += fDeltaTime;
			fTimeToStop += fDeltaTime;
			fTimeToPropagade += fDeltaTime;
			break;
		}
		if (fTime >= fTimeToStop)
		{
			// STOP
			m_current_state = stStopped;
			SoundRender->i_stop(this);
		}
		else
		{
			if (!update_culling(dt))
			{
				// switch to: SIMULATE
				m_current_state = stSimulating; // switch state
				SoundRender->i_stop(this);
			}
			else
			{
				// We are still playing
				update_environment(dt);
			}
		}
		break;
	case stSimulating:
		if (iPaused)
		{
			fTimeStarted += fDeltaTime;
			fTimeToStop += fDeltaTime;
			fTimeToPropagade += fDeltaTime;
			break;
		}
		if (fTime >= fTimeToStop)
		{
			// STOP
			m_current_state = stStopped;
		}
		else
		{
			u32 ptr = calc_cursor(fTimeStarted,
			                      fTime,
			                      get_length_sec(),
			                      source()->m_wformat);
			set_cursor(ptr);

			if (update_culling(dt))
			{
				// switch to: PLAY
				m_current_state = stPlaying;
				/*
								u32 ptr						= calc_cursor(	fTimeStarted, 
																			fTime, 
																			get_length_sec(), 
																			source()->m_wformat); 
								set_cursor					(ptr);
				*/
				SoundRender->i_start(this);
			}
		}
		break;
	case stPlayingLooped:
		if (iPaused)
		{
			if (target)
			{
				SoundRender->i_stop(this);
				m_current_state = stSimulatingLooped;
			}
			fTimeStarted += fDeltaTime;
			fTimeToPropagade += fDeltaTime;
			break;
		}
		if (!update_culling(dt))
		{
			// switch to: SIMULATE
			m_current_state = stSimulatingLooped; // switch state
			SoundRender->i_stop(this);
		}
		else
		{
			// We are still playing
			update_environment(dt);
		}
		break;
	case stSimulatingLooped:
		if (iPaused)
		{
			fTimeStarted += fDeltaTime;
			fTimeToPropagade += fDeltaTime;
			break;
		}
		if (update_culling(dt))
		{
			// switch to: PLAY
			m_current_state = stPlayingLooped; // switch state
			u32 ptr = calc_cursor(fTimeStarted,
			                      fTime,
			                      get_length_sec(),
			                      source()->m_wformat);
			set_cursor(ptr);

			SoundRender->i_start(this);
		}
		break;
	}

#ifdef MEASURE_PROCESSING_TIME
	float time2 = Timer.GetElapsed_ms_f();
#endif

	// if deffered stop active and volume==0 -> physically stop sound
	if (bStopping && fis_zero(fade_volume))
		i_stop();

	VERIFY2(!!(owner_data) || (!(owner_data)&&(m_current_state==stStopped)), "owner");
	VERIFY2(owner_data?*(int*)(owner_data->feedback):1, "owner");

	// footer
	bMoved = FALSE;
	if (m_current_state != stStopped)
	{
		if (fTime >= fTimeToPropagade)
			Event_Propagade();
	}
	else if (owner_data)
	{
		VERIFY(this==owner_data->feedback);
		owner_data->feedback = 0;
		owner_data = 0;
	}
	
#ifdef MEASURE_PROCESSING_TIME
	float time3 = Timer.GetElapsed_ms_f();
	const char* name = "?";
	if (owner_data && source())
	{
		name = source()->fname.c_str();
	}
	float d1 = time2 - time1;
	float d2 = time3 - time2;
	if (d1 > 1 || d2 > 1)
	{
		Msg("|SND| %s took long: %f %f", name, d1, d2);
	}
#endif
}

IC void volume_lerp(float& c, float t, float s, float dt)
{
	float diff = t - c;
	float diff_a = _abs(diff);
	if (diff_a < EPS_S) return;
	float mot = s * dt;
	if (mot > diff_a) mot = diff_a;
	c += (diff / diff_a) * mot;
}

#include "..\xrServerEntities\ai_sounds.h"

BOOL CSoundRender_Emitter::update_culling(float dt)
{
	float volume_att = 1.f;

	if (b2D)
	{
		occluder_volume = 1.f;

		// Configurable fade-out (stop_deffered) / fade-in (post-play), driven by elapsed
		// time rather than accumulation so duration and curve are precise and independent
		// of frame rate. Defaults (0.1s / linear / fade-in disabled) exactly reproduce the
		// original hardcoded dt*10.f ramp for any sound that never calls
		// set_fade_out/set_fade_in.
		const float HALF_PI = 1.5707963267948966f;
		if (bStopping)
		{
			fade_out_elapsed += dt;
			float dur = (fade_out_duration_s > 0.f) ? fade_out_duration_s : 0.1f;
			float p = fade_out_elapsed / dur;
			clamp(p, 0.f, 1.f);
			if (p >= 1.f) fade_volume = 0.f;
			else fade_volume = (fade_out_curve == 1) ? cosf(p * HALF_PI) : (1.f - p);
		}
		else if (fade_in_duration_s > 0.f)
		{
			fade_in_elapsed += dt;
			float p = fade_in_elapsed / fade_in_duration_s;
			clamp(p, 0.f, 1.f);
			if (p >= 1.f) fade_volume = 1.f;
			else fade_volume = (fade_in_curve == 1) ? sinf(p * HALF_PI) : p;
		}
		else
		{
			fade_volume = 1.f;
		}

		volume_att = p_source.volume;
	}
	else
	{
		// Phase 0 occlusion instrumentation (29/09): every 3D emitter that reaches this branch this
		// frame, regardless of whether it goes on to call get_occlusion (world_ambient/culled ones
		// won't) -- a simple "how many active 3D sounds" gauge alongside the occlusion call/ray counts.
		SoundRender->m_occ_cur.emitters_3d++;

		// Check range
		float dist = SoundRender->listener_position().distance_to(p_source.position);
		if (dist > p_source.max_distance)
		{
			smooth_volume = 0;
			return FALSE;
		}

		// Calc attenuated volume
		//LostAlphaRus in
		float min_max = p_source.max_distance - p_source.min_distance;
		volume_att = (p_source.max_distance - dist) / min_max;
		clamp(volume_att, 0.f, p_source.volume);

		float fade_scale = bStopping || (p_source.base_volume * p_source.volume * (owner_data->s_type == st_Effect ? psSoundVEffects * psSoundVFactor : psSoundVMusic * psSoundVMusicFactor) < psSoundCull) ? -1.f : 1.f;
		fade_volume += dt * 10.f * fade_scale;
		//LostAlphaRus out

		//v2v3v4 in
		if (dist > p_source.max_distance)
			volume_att -= 0.1f;
		//v2v3v4 out

		// Update occlusion
		if (owner_data->g_type == SOUND_TYPE_WORLD_AMBIENT)
		{
			occluder_volume = 1.f;
			occluder_gain_hf = 1.f;
			occluder_gain_wet = 1.f;
		}
		else if (psSoundOcclusionMode == 0)
		{
			// Original path, byte-identical to before Phase 0/1/2 existed.
			float occ = SoundRender->get_occlusion(p_source.position, .2f, occluder);
			volume_lerp(occluder_volume, occ, 1.f, dt);
			clamp(occluder_volume, 0.f, 1.f);
		}
		else if (occ_is_loop)
		{
			// Phase 1/2 (29-30/09): cadence-gated (snd_occlusion_update_ms) and budget-limited
			// (snd_occlusion_budget) full re-evaluation, smoothly followed at ~4/s in between --
			// faster than mode 0's fixed 1.0/s since a listener/NPC can genuinely move behind cover
			// mid-sustain, and this path is already far cheaper per-call than mode 0 fears.
			// Update (01/10): only reached for looped voices -- see the one-shot branch below.
			const float now = SoundRender->fTimer_Value;
			if (now >= occ_next_update && SoundRender->occ_budget_take())
			{
				const bool debug_this = occ_profile == 1;
				CSoundRender_Core::SSoundOcclusionResult r = SoundRender->get_occlusion_ex(p_source.position,
					occ_profile, debug_this && source() ? source()->file_name() : nullptr, debug_this);
				occ_target_gain = r.gain;
				occ_target_hf = r.gain_hf;
				occ_target_wet_gain = r.wet_gain;
				occ_next_update = now + _max(psSoundOcclusionUpdateMs, 0) / 1000.f;
			}
			volume_lerp(occluder_volume, occ_target_gain, 4.f, dt);
			volume_lerp(occluder_gain_hf, occ_target_hf, 4.f, dt);
			volume_lerp(occluder_gain_wet, occ_target_wet_gain, 4.f, dt);
			clamp(occluder_volume, 0.f, 1.f);
			clamp(occluder_gain_hf, 0.f, 1.f);
			clamp(occluder_gain_wet, 0.f, 1.f);
		}
		// else (01/10): one-shot voice (occ_is_loop == false) -- occluder_volume/gain_hf/gain_wet already
		// hold the single result stStarting computed the instant this sound was emitted, and are
		// deliberately left untouched for the rest of this voice's playback. A sound already in flight
		// doesn't retroactively re-route itself around an obstacle the listener ducks behind afterward,
		// and a one-shot's whole lifetime (a gunshot's crack+tail) is far too short for the listener's
		// real position to have moved meaningfully anyway -- continuously re-evaluating it only produced
		// an unrealistic "instant mute the moment I crouch" snap. Looped voices (the branch above) keep
		// full continuous tracking, since they genuinely can outlast the listener staying in one place.

		// Actor-fire priority ducking (30/09, "snd_duck_mode 1"). Independent of occlusion mode -- applies
		// on top of whatever occlusion already computed above. Only gunshot/explosion-profile voices are a
		// target (occ_profile == 1, resolved once in start()); the actor's own weapon sound never reaches
		// this 3D branch at all (it plays through the b2D branch above), so it can never duck itself.
		// duck_target flips between a strength/loudness-scaled value and 1.0 based on elapsed time since
		// the actor's last shot (psSoundDuckHoldMs) -- volume_lerp (with a fast attack, slow release rate)
		// turns that flip into a click-free ramp, same primitive already proven for occlusion smoothing.
		if (psSoundDuckMode && occ_profile == 1)
		{
			const float loudness_weight = _min(_max(volume_att * occluder_volume, 0.f), 1.f);
			const float since_last_shot_ms = (SoundRender->fTimer_Value - SoundRender->m_actor_last_shot_time) * 1000.f;
			const bool duck_active = since_last_shot_ms < psSoundDuckHoldMs;
			const float duck_target = duck_active ? (1.f - psSoundDuckStrength * loudness_weight) : 1.f;
			const float rate = (duck_target < duck_gain) ? psSoundDuckAttackRate : psSoundDuckReleaseRate;
			volume_lerp(duck_gain, duck_target, rate, dt);
			clamp(duck_gain, 0.f, 1.f);
		}
		else
		{
			duck_gain = 1.f;
		}
	}
	clamp(fade_volume, 0.f, 1.f);

	// Update smoothing
	//LostAlphaRus in
	// Mode 1, 3D: occlusion no longer multiplies AL_GAIN -- see the stStarting/stStartingLooped cases
	// above for why (decouples the direct path from the reverb send, which has its own gain instead).
	const float occ_for_gain = (psSoundOcclusionMode == 1 && !b2D) ? 1.f : occluder_volume;
	// duck_gain (30/09) is 1.f for every 2D sound and every non-gunshot 3D sound (see update_culling),
	// so this multiply is a no-op unless snd_duck_mode is on and this voice is an active NPC gunshot.
	smooth_volume = (p_source.base_volume * volume_att * (owner_data->s_type == st_Effect ? psSoundVEffects * psSoundVFactor : psSoundVMusic * psSoundVMusicFactor) * occ_for_gain * fade_volume * duck_gain);
	//LostAlphaRus out

	if (smooth_volume < psSoundCull)
		return FALSE;	// allow volume to go up

	// Here we has enought "PRIORITY" to be soundable
	// If we are playing already, return OK
	// --- else check availability of resources
	if (target)
		return TRUE;

	return SoundRender->i_allow_play(this);
}

float CSoundRender_Emitter::priority()
{
	float volume_att = 1.f;

	float dist = SoundRender->listener_position().distance_to(p_source.position);

	if (b2D)
	{
		volume_att = p_source.min_distance / (psSoundRolloff * dist);
		clamp(volume_att, 0.f, 1.f);
	}
	else
	{
		float min_max = p_source.max_distance - p_source.min_distance;
		volume_att = (p_source.max_distance - dist) / min_max;
		clamp(volume_att, 0.f, p_source.volume);
	}

	return	smooth_volume * volume_att * priority_scale;
}

void CSoundRender_Emitter::update_environment(float dt)
{
	if (bMoved) {
		p_source.update_velocity(dt);
	}
}
