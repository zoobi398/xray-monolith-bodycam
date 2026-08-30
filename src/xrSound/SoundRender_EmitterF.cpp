#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_EmitterF.h"
#include "SoundRender_CoreF.h"
#include "SoundRender_Source.h"

#include <fmod.hpp>
#include <fmod_errors.h>

xr_vector<CSoundRender_EmitterF*> CSoundRender_EmitterF::s_all;

CSoundRender_EmitterF::CSoundRender_EmitterF()
	: m_sound(nullptr), m_channel(nullptr), owner_data(nullptr),
	  m_stopping(FALSE), m_finished(false),
	  m_fade_out_duration_s(0.1f), m_fade_in_duration_s(0.f),
	  m_fade_out_curve(0), m_fade_in_curve(0),
	  m_fade_out_elapsed(0.f), m_fade_in_elapsed(0.f), m_fade_volume(1.f)
{
	m_params.volume = 1.f;
}

CSoundRender_EmitterF::~CSoundRender_EmitterF()
{
	i_stop();
}

void CSoundRender_EmitterF::start(ref_sound* owner, BOOL loop, float delay)
{
	owner_data = owner->_p;

	CSound_source* src = owner->_handle();

	// Mirror CSoundRender_Source::load()'s path resolution exactly, since
	// file_name() only returns the lowercased, extension-stripped logical
	// name, not a ready-to-open path.
	string_path fn;
	strconcat(sizeof(fn), fn, src->file_name(), ".ogg");
	if (!FS.exist("$level$", fn))
		FS.update_path(fn, "$game_sounds$", fn);
	if (!FS.exist(fn))
		FS.update_path(fn, "$game_sounds$", "$no_sound.ogg");

	FMOD::System* sys = FMODCore_System();
	if (!sys)
	{
		i_stop();
		return;
	}

	// Read via FS (not FMOD's own file handling): gamedata can be loose files
	// or packed inside a .db archive, and only X-Ray's own IReader knows how
	// to transparently see through both. A raw path handed straight to
	// FMOD::createSound() only ever resolves for loose files.
	IReader* reader = FS.r_open(fn);
	if (!reader || !reader->length())
	{
		Msg("! FMOD: could not open '%s' via FS", fn);
		if (reader) FS.r_close(reader);
		i_stop();
		return;
	}

	FMOD_CREATESOUNDEXINFO exinfo = {};
	exinfo.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
	exinfo.length = (unsigned int)reader->length();

	FMOD_MODE mode = FMOD_DEFAULT | FMOD_OPENMEMORY | (loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF);
	FMOD_RESULT result = sys->createSound((const char*)reader->pointer(), mode, &exinfo, &m_sound);
	FS.r_close(reader);
	if (result != FMOD_OK)
	{
		Msg("! FMOD: createSound (memory) failed for '%s': %s", fn, FMOD_ErrorString(result));
		i_stop();
		return;
	}

	result = sys->playSound(m_sound, nullptr, false, &m_channel);
	if (result != FMOD_OK)
	{
		Msg("! FMOD: playSound failed for '%s': %s", fn, FMOD_ErrorString(result));
		i_stop();
		return;
	}

	apply_volume();
	s_all.push_back(this);
}

void CSoundRender_EmitterF::apply_volume()
{
	if (!m_channel) return;

	// Mirror CSoundRender_Emitter's smooth_volume computation: apply the
	// master category slider (Effects or Music, from the options menu) on
	// top of the per-instance volume. Without this, FMOD-routed 2D sounds
	// (music included, since it's typically 2D) ignore both sliders entirely.
	float category_volume = (owner_data && owner_data->s_type == st_Music)
		? (psSoundVMusic * psSoundVMusicFactor)
		: (psSoundVEffects * psSoundVFactor);

	m_channel->setVolume(m_params.volume * category_volume * m_fade_volume);
}

void CSoundRender_EmitterF::set_volume(float vol)
{
	if (!_valid(vol)) vol = 0.f;
	m_params.volume = vol;
	apply_volume();
}

void CSoundRender_EmitterF::set_frequency(float freq)
{
	VERIFY(_valid(freq));
	m_params.freq = freq;
	if (m_channel) m_channel->setPitch(freq);
}

void CSoundRender_EmitterF::set_fade_out(float duration_s, int curve)
{
	m_fade_out_duration_s = (duration_s > 0.f) ? duration_s : 0.1f;
	m_fade_out_curve = curve;
}

void CSoundRender_EmitterF::set_fade_in(float duration_s, int curve)
{
	m_fade_in_duration_s = (duration_s > 0.f) ? duration_s : 0.f;
	m_fade_in_curve = curve;
	if (m_fade_in_duration_s > 0.f)
	{
		m_fade_in_elapsed = 0.f;
		m_fade_volume = 0.f;
		apply_volume();
	}
}

void CSoundRender_EmitterF::stop(BOOL bDeffered)
{
	if (bDeffered)
	{
		m_stopping = TRUE;
		m_fade_out_elapsed = 0.f;
	}
	else
	{
		i_stop();
	}
}

void CSoundRender_EmitterF::rewind()
{
	if (m_channel) m_channel->setPosition(0, FMOD_TIMEUNIT_MS);
	m_stopping = FALSE;
	m_fade_out_elapsed = 0.f;
	m_fade_in_elapsed = 0.f;
	m_fade_volume = (m_fade_in_duration_s > 0.f) ? 0.f : 1.f;
	apply_volume();
}

u32 CSoundRender_EmitterF::play_time()
{
	if (!m_channel) return 0;
	unsigned int pos = 0;
	m_channel->getPosition(&pos, FMOD_TIMEUNIT_MS);
	return (u32)pos;
}

void CSoundRender_EmitterF::update(float dt)
{
	if (m_finished || !m_channel) return;

	bool is_playing = false;
	m_channel->isPlaying(&is_playing);
	if (!is_playing)
	{
		i_stop();
		return;
	}

	const float HALF_PI = 1.5707963267948966f;
	if (m_stopping)
	{
		m_fade_out_elapsed += dt;
		float dur = (m_fade_out_duration_s > 0.f) ? m_fade_out_duration_s : 0.1f;
		float p = m_fade_out_elapsed / dur;
		clamp(p, 0.f, 1.f);
		m_fade_volume = (m_fade_out_curve == 1) ? cosf(p * HALF_PI) : (1.f - p);
		if (p >= 1.f)
		{
			apply_volume();
			i_stop();
			return;
		}
	}
	else if (m_fade_in_duration_s > 0.f && m_fade_in_elapsed < m_fade_in_duration_s)
	{
		m_fade_in_elapsed += dt;
		float p = m_fade_in_elapsed / m_fade_in_duration_s;
		clamp(p, 0.f, 1.f);
		m_fade_volume = (m_fade_in_curve == 1) ? sinf(p * HALF_PI) : p;
	}

	// Always reapply, not just on a fade tick, so a steady-state looping
	// sound (music included) keeps tracking the Effects/Music sliders live
	// while the options menu is open, exactly like the OpenAL path does.
	apply_volume();
}

void CSoundRender_EmitterF::i_stop()
{
	if (m_finished) return;

	if (m_channel)
	{
		m_channel->stop();
		m_channel = nullptr;
	}
	if (m_sound)
	{
		m_sound->release();
		m_sound = nullptr;
	}
	if (owner_data)
	{
		owner_data->feedback = nullptr;
		owner_data = nullptr;
	}
	m_finished = true;
}

void CSoundRender_EmitterF::update_all(float dt)
{
	for (u32 i = 0; i < s_all.size(); ++i)
		s_all[i]->update(dt);

	for (int i = (int)s_all.size() - 1; i >= 0; --i)
	{
		if (s_all[i]->m_finished)
		{
			xr_delete(s_all[i]);
			s_all.erase(s_all.begin() + i);
		}
	}
}

void CSoundRender_EmitterF::destroy_all()
{
	for (u32 i = 0; i < s_all.size(); ++i)
	{
		s_all[i]->i_stop();
		xr_delete(s_all[i]);
	}
	s_all.clear();
}
