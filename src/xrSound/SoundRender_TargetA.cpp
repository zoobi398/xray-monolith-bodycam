#include "stdafx.h"
#pragma hdrstop

#include "soundrender_TargetA.h"
#include "soundrender_emitter.h"
#include "soundrender_source.h"

xr_vector<u8> g_target_temp_data;
xr_vector<u8> g_target_temp_data_16;

CSoundRender_TargetA::CSoundRender_TargetA(): CSoundRender_Target()
{
	cache_gain = 0.f;
	cache_pitch = 1.f;
	pSource = 0;
	Slot = u32(-1);
	m_direct_filter = 0;
	m_send_filter = 0;
	cache_hf = -1.f;
	cache_gain_direct = -1.f;
	cache_gain_wet = -1.f;
}

CSoundRender_TargetA::~CSoundRender_TargetA()
{
}

void CSoundRender_TargetA::SetSlot(ALuint NewSlot)
{
	Slot = NewSlot;
}

BOOL CSoundRender_TargetA::_initialize()
{
	inherited::_initialize();
	// initialize buffer
	A_CHK(alGenBuffers (sdef_target_count, pBuffers));
	alGenSources(1, &pSource);
	ALenum error = alGetError();
	if (AL_NO_ERROR == error)
	{
		A_CHK(alSourcei (pSource, AL_LOOPING, AL_FALSE));
		A_CHK(alSourcef (pSource, AL_MIN_GAIN, 0.f));
		A_CHK(alSourcef (pSource, AL_MAX_GAIN, 1.f));
		A_CHK(alSourcef (pSource, AL_GAIN, cache_gain));
		A_CHK(alSourcef (pSource, AL_PITCH, cache_pitch));

		// Phase 1/2 occlusion rework (29/09): create both low-pass filters once per pooled target,
		// regardless of the current snd_occlusion_mode, so the mode can be toggled live later without
		// needing to recreate targets. Never bound to a source unless mode 1 is active (see render()).
		// occ_gen_filter/occ_delete_filter/occ_set_filter_lowpass are passthroughs on SoundRender --
		// the actual alGenFilters/etc. function pointers are private to CSoundRender_CoreA.
		m_direct_filter = SoundRender->occ_gen_filter();
		m_send_filter = SoundRender->occ_gen_filter();
		return TRUE;
	}
	else
	{
		Msg("! sound: OpenAL: Can't create source. Error: %s.", (LPCSTR)alGetString(error));
		return FALSE;
	}
}

void CSoundRender_TargetA::_destroy()
{
	// clean up target
	if (alIsSource(pSource))
		alDeleteSources(1, &pSource);
	A_CHK(alDeleteBuffers (sdef_target_count, pBuffers));
	SoundRender->occ_delete_filter(m_direct_filter);
	SoundRender->occ_delete_filter(m_send_filter);
	m_direct_filter = 0;
	m_send_filter = 0;
	inherited::_destroy();
}

void CSoundRender_TargetA::_restart()
{
	_destroy();
	_initialize();
}

void CSoundRender_TargetA::start(CSoundRender_Emitter* E)
{
	inherited::start(E);

	// Calc storage
	buf_block = sdef_target_block * E->source()->m_wformat.nAvgBytesPerSec / 1000;
	g_target_temp_data.resize(buf_block);
	g_target_temp_data_16.resize(buf_block * 2);
}

void CSoundRender_TargetA::render()
{
	for (u32 buf_idx = 0; buf_idx < sdef_target_count; buf_idx++)
		fill_block(pBuffers[buf_idx]);

	// Phase 1/2 occlusion rework (29/09): only bind the low-pass filters for this voice's lifetime when
	// mode 1 is active AND it's a 3D emitter (2D/HUD sounds are never occluded, matching mode 0). A
	// mode toggle only affects sounds that (re)start after the toggle, same lifecycle as Slot below.
	const bool use_occ_filter = SoundRender->m_is_supported && psSoundOcclusionMode == 1 && !m_pEmitter->b2D;
	// Force the first fill_parameters() call to actually push values, not skip on a stale cache match.
	cache_hf = -1.f;
	cache_gain_direct = -1.f;
	cache_gain_wet = -1.f;
	A_CHK(alSourcei(pSource, AL_DIRECT_FILTER, use_occ_filter ? m_direct_filter : AL_FILTER_NULL));

	A_CHK(alSourceQueueBuffers(pSource, sdef_target_count, pBuffers));
	if (Slot != u32(-1) && !m_pEmitter->bIntro)
	{
		A_CHK(alSource3i(pSource, AL_AUXILIARY_SEND_FILTER, Slot, 0, use_occ_filter ? m_send_filter : AL_FILTER_NULL));
	}
	// demonized: explicitly disable effects by sending sounds to null slot, ie. not sending
	else
	{
		A_CHK(alSource3i(pSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, NULL));
	}
	A_CHK(alSourcePlay(pSource));

	inherited::render();
}

void CSoundRender_TargetA::stop()
{
	if (rendering)
	{
		A_CHK(alSourceStop(pSource));
		A_CHK(alSourcei (pSource, AL_BUFFER, NULL));
		A_CHK(alSourcei (pSource, AL_SOURCE_RELATIVE, TRUE));
	}
	inherited::stop();
}

void CSoundRender_TargetA::rewind()
{
	inherited::rewind();

	A_CHK(alSourceStop(pSource));
	A_CHK(alSourcei (pSource, AL_BUFFER, NULL));
	for (u32 buf_idx = 0; buf_idx < sdef_target_count; buf_idx++)
		fill_block(pBuffers[buf_idx]);
	A_CHK(alSourceQueueBuffers (pSource, sdef_target_count, pBuffers));
	A_CHK(alSourcePlay (pSource));
}

void CSoundRender_TargetA::update()
{
	inherited::update();

	ALint processed;
	// Get status
	A_CHK(alGetSourcei(pSource, AL_BUFFERS_PROCESSED, &processed));

	if (processed > 0)
	{
		while (processed)
		{
			ALuint BufferID;
			A_CHK(alSourceUnqueueBuffers(pSource, 1, &BufferID));
			fill_block(BufferID);
			A_CHK(alSourceQueueBuffers(pSource, 1, &BufferID));
			--processed;
		}

		ALint state;
		A_CHK(alGetSourcei(pSource, AL_SOURCE_STATE, &state));
		if (state == AL_STOPPED)
		A_CHK(alSourcePlay(pSource));
	}
	else
	{
		// processed == 0
		// check play status -- if stopped then queue is not being filled fast enough
		ALint state;
		A_CHK(alGetSourcei(pSource, AL_SOURCE_STATE, &state));
		if (state != AL_PLAYING)
		{
			//			Log		("Queuing underrun detected.");
			A_CHK(alSourcePlay(pSource));
		}
	}
}

void CSoundRender_TargetA::fill_parameters()
{
	CSoundRender_Emitter* SE = m_pEmitter;
	VERIFY(SE);

	inherited::fill_parameters();

	// 3D params
	VERIFY2(m_pEmitter, SE->source()->file_name());
	A_CHK(alSourcef (pSource, AL_REFERENCE_DISTANCE, m_pEmitter->p_source.min_distance));

	VERIFY2(m_pEmitter, SE->source()->file_name());
	A_CHK(alSourcef (pSource, AL_MAX_DISTANCE, m_pEmitter->p_source.max_distance));

	VERIFY2(m_pEmitter, SE->source()->file_name ());
	A_CHK(alSource3f(pSource, AL_POSITION, m_pEmitter->p_source.position.x,m_pEmitter->p_source.position.y,-m_pEmitter->
		p_source.position.z));

	VERIFY2(m_pEmitter, SE->source()->file_name());
	A_CHK(alSource3f(pSource, AL_VELOCITY, m_pEmitter->p_source.velocity.x, m_pEmitter->p_source.velocity.y, -m_pEmitter->p_source.velocity.z));

	VERIFY2(m_pEmitter, SE->source()->file_name());
	A_CHK(alSourcei (pSource, AL_SOURCE_RELATIVE, m_pEmitter->b2D));

	A_CHK(alSourcef (pSource, AL_ROLLOFF_FACTOR, psSoundRolloff));

	VERIFY2(m_pEmitter, SE->source()->file_name());
	float _gain = m_pEmitter->smooth_volume;
	clamp(_gain, EPS_S, 1.f);
	if (!fsimilar(_gain, cache_gain, 0.01f))
	{
		cache_gain = _gain;
		A_CHK(alSourcef (pSource, AL_GAIN, _gain));
	}

	// Phase 1/2 occlusion rework (29-30/09): live low-pass update. The filter OBJECTS are already bound
	// to the source for this voice's whole lifetime (render(), above) whenever mode 1 applies to it --
	// modifying their parameters here takes effect immediately without re-binding anything.
	// 30/09: the direct and send filters now get INDEPENDENT broadband gains (occluder_volume vs
	// occluder_gain_wet -- see SoundRender_Emitter_FSM.cpp) instead of both riding on the shared
	// AL_GAIN. This is what actually fixes occlusion feeling like a mute switch indoors: a real
	// obstacle chokes the direct signal, but the room's own reflected energy (the reverb send) is only
	// partly affected, governed by snd_occlusion_wet_sensitivity. The send filter still gets a milder
	// (sqrt) HF cut on top -- reflected sound loses less treble than the direct path around an obstacle.
	if (SoundRender->m_is_supported && psSoundOcclusionMode == 1 && !m_pEmitter->b2D)
	{
		float hf = m_pEmitter->occluder_gain_hf;
		clamp(hf, 0.01f, 1.f);
		float gain_direct = m_pEmitter->occluder_volume;
		clamp(gain_direct, 0.01f, 1.f);
		float gain_wet = m_pEmitter->occluder_gain_wet;
		clamp(gain_wet, 0.01f, 1.f);

		if (!fsimilar(hf, cache_hf, 0.01f) || !fsimilar(gain_direct, cache_gain_direct, 0.01f) ||
			!fsimilar(gain_wet, cache_gain_wet, 0.01f))
		{
			cache_hf = hf;
			cache_gain_direct = gain_direct;
			cache_gain_wet = gain_wet;
			SoundRender->occ_set_filter_lowpass(m_direct_filter, gain_direct, hf);
			SoundRender->occ_set_filter_lowpass(m_send_filter, gain_wet, sqrtf(hf));
		}
	}

	VERIFY2(m_pEmitter, SE->source()->file_name());
	float _pitch = m_pEmitter->p_source.freq;
	clamp(_pitch, EPS_L, 2.f);

	if (!fsimilar(cache_pitch, _pitch * psSpeedOfSound))
	{
		cache_pitch = _pitch * psSpeedOfSound;

		// Only update time to stop for non-looped sounds
		if (!m_pEmitter->iPaused && (m_pEmitter->m_current_state == CSoundRender_Emitter::stStarting || m_pEmitter->m_current_state == CSoundRender_Emitter::stPlaying || m_pEmitter->m_current_state == CSoundRender_Emitter::stSimulating))
			m_pEmitter->fTimeToStop = SoundRender->fTimer_Value + ((m_pEmitter->get_length_sec() - (SoundRender->fTimer_Value - m_pEmitter->fTimeStarted)) / cache_pitch);

		A_CHK(alSourcef(pSource, AL_PITCH, cache_pitch));
	}
	VERIFY2(m_pEmitter, SE->source()->file_name());
}

void CSoundRender_TargetA::fill_block(ALuint BufferID)
{
	R_ASSERT(m_pEmitter);
	ALuint format = (m_pEmitter->source()->m_wformat.nChannels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	if (format == AL_FORMAT_MONO16)
	{
		m_pEmitter->fill_block(&g_target_temp_data.front(), g_target_temp_data.size());
		A_CHK(alBufferData(BufferID, format, &g_target_temp_data.front(), g_target_temp_data.size(), m_pEmitter->source()->m_wformat.nSamplesPerSec));
	}
	else
	{
		m_pEmitter->fill_block(&g_target_temp_data_16.front(), g_target_temp_data_16.size());
		A_CHK(alBufferData(BufferID, format, &g_target_temp_data_16.front(), g_target_temp_data_16.size(), m_pEmitter->source()->m_wformat.nSamplesPerSec));
	}
}

void CSoundRender_TargetA::source_changed()
{
	dettach();
	attach();
}
