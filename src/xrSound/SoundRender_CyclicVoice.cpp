#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_Core.h"
#include "SoundRender_CyclicVoice.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <chrono>
#include <AL/efx.h>

// Ogg callbacks defined (with external linkage) in SoundRender_Source_loader.cpp
int ov_seek_func(void* datasource, s64 offset, int whence);
size_t ov_read_func(void* ptr, size_t size, size_t nmemb, void* datasource);
int ov_close_func(void* datasource);
long ov_tell_func(void* datasource);

namespace
{
	const float HALF_PI = 1.5707963267948966f;
	const double NS = 1.0e9;
	const size_t PCM_CACHE_LIMIT = 96u * 1024u * 1024u;

	inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

CSoundRender_CyclicVoice::CSoundRender_CyclicVoice() {}

CSoundRender_CyclicVoice::~CSoundRender_CyclicVoice() { destroy(); }

void CSoundRender_CyclicVoice::dbg(const char* fmt, ...) const
{
	if (!psSoundCyclicDebug) return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Msg("* [cyclic] %s", buf);
}

// The OpenAL device clock is a step function (it moves once per mixer update, about every 10 ms), so reading it
// as "the time of the shot" blurs every shot time by up to one step. Pair it with a high-resolution timer instead:
// device_time(now) = real_time(now) + offset, where the offset is the MAX of (device - real) over the last
// seconds (a stale reading can only be behind the true device time, never ahead).
CSoundRender_CyclicVoice::clk_t CSoundRender_CyclicVoice::now_ns()
{
	ALCint64SOFT v = 0;
	if (m_alcGetInteger64vSOFT && m_dev)
		m_alcGetInteger64vSOFT(m_dev, ALC_DEVICE_CLOCK_SOFT, 1, &v);
	const s64 r = (s64)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	m_clk_r[m_clk_head] = r;
	m_clk_off[m_clk_head] = (s64)v - r;
	m_clk_head = (m_clk_head + 1) % CLK_RING;
	if (m_clk_n < CLK_RING) ++m_clk_n;
	s64 best = m_clk_off[(m_clk_head + CLK_RING - 1) % CLK_RING];
	const s64 horizon = r - (s64)(3.0 * NS);
	for (int i = 0; i < m_clk_n; ++i)
		if (m_clk_r[i] >= horizon && m_clk_off[i] > best) best = m_clk_off[i];
	return (clk_t)(r + best);
}

// snd_cyclic_pipeline_ms is a minimum: a shot is only known once its frame is processed, so the delay can never
// be shorter than about one frame without the queue running dry between two blocks.
CSoundRender_CyclicVoice::clk_t CSoundRender_CyclicVoice::latency_ns() const
{
	const float ms = std::max(psSoundCyclicPipelineMs, 1.3f * m_frame_ms);
	return (clk_t)(ms * 1.0e6f);
}

float CSoundRender_CyclicVoice::effects_volume() const
{
	return psSoundVEffects * psSoundVFactor;
}

// ---------------------------------------------------------------------------------------------------------------
// init / destroy
// ---------------------------------------------------------------------------------------------------------------

void CSoundRender_CyclicVoice::init(ALCdevice* dev, ALuint efx_slot)
{
	std::lock_guard<std::mutex> g(m_lock);
	m_ready = false;
	m_dev = dev;
	m_efx_slot = efx_slot;
	m_pool = 0;
	m_s = SSession();
	m_clk_n = 0;
	m_clk_head = 0;

	if (!dev) return;
	if (!alcIsExtensionPresent(dev, "ALC_SOFT_device_clock") || !alIsExtensionPresent("AL_SOFT_source_start_delay"))
	{
		Msg("SOUND: cyclic voice disabled: OpenAL Soft clock / start-delay extensions missing");
		return;
	}
	m_alcGetInteger64vSOFT = (LPALCGETINTEGER64VSOFT)alcGetProcAddress(dev, "alcGetInteger64vSOFT");
	m_alSourcePlayAtTimeSOFT = (LPALSOURCEPLAYATTIMESOFT)alGetProcAddress("alSourcePlayAtTimeSOFT");
	if (!m_alcGetInteger64vSOFT || !m_alSourcePlayAtTimeSOFT)
	{
		Msg("SOUND: cyclic voice disabled: could not load the OpenAL Soft clock functions");
		return;
	}

	alGetError();
	for (int i = 0; i < POOL; ++i)
	{
		SSlot& s = m_slot[i];
		s = SSlot();
		alGenSources(1, &s.src);
		if (alGetError() != AL_NO_ERROR)
		{
			s.src = 0;
			break;
		}
		alSourcei(s.src, AL_LOOPING, AL_FALSE);
		alSourcei(s.src, AL_SOURCE_RELATIVE, AL_TRUE);
		alSource3f(s.src, AL_POSITION, 0.f, 0.f, 0.f);
		alSourcef(s.src, AL_ROLLOFF_FACTOR, 0.f);
		alSourcef(s.src, AL_MIN_GAIN, 0.f);
		alSourcef(s.src, AL_MAX_GAIN, 1.f);
		alSourcef(s.src, AL_GAIN, 0.f);
		alSourcef(s.src, AL_PITCH, 1.f);
		if (m_efx_slot)
			alSource3i(s.src, AL_AUXILIARY_SEND_FILTER, (ALint)m_efx_slot, 0, 0 /*AL_FILTER_NULL*/);
		++m_pool;
	}
	if (m_pool < 4)
	{
		Msg("SOUND: cyclic voice disabled: only %d OpenAL sources available", m_pool);
		for (int i = 0; i < m_pool; ++i)
			alDeleteSources(1, &m_slot[i].src);
		m_pool = 0;
		return;
	}
	m_ready = true;
	Msg("SOUND: cyclic voice ready (%d sources)", m_pool);
}

void CSoundRender_CyclicVoice::destroy()
{
	std::lock_guard<std::mutex> g(m_lock);
	if (m_dev)
	{
		for (int i = 0; i < m_pool; ++i)
		{
			SSlot& s = m_slot[i];
			if (s.src)
			{
				alSourceStop(s.src);
				alSourcei(s.src, AL_BUFFER, 0);
				alDeleteSources(1, &s.src);
				s.src = 0;
			}
		}
		free_pcm_all();
	}
	m_pool = 0;
	m_ready = false;
	m_s = SSession();
	m_dev = nullptr;
}

void CSoundRender_CyclicVoice::free_pcm_all()
{
	for (auto& kv : m_pcm)
	{
		SPcm* p = kv.second;
		if (!p) continue;
		if (p->whole) alDeleteBuffers(1, &p->whole);
		for (auto& sl : p->slices) alDeleteBuffers(1, &sl.second);
		delete p;
	}
	m_pcm.clear();
	m_pcm_bytes = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// samples
// ---------------------------------------------------------------------------------------------------------------

bool CSoundRender_CyclicVoice::load_pcm(const char* name, SPcm& out)
{
	string_path fn, N;
	xr_strcpy(N, name);
	strlwr(N);
	if (strext(N)) *strext(N) = 0;
	strconcat(sizeof(fn), fn, N, ".ogg");
	if (!FS.exist("$level$", fn))
		FS.update_path(fn, "$game_sounds$", fn);
	if (!FS.exist(fn))
	{
		Msg("! cyclic voice: can't find sound '%s'", name);
		return false;
	}

	IReader* wave = FS.r_open(fn);
	if (!wave || !wave->length())
	{
		if (wave) FS.r_close(wave);
		return false;
	}
	OggVorbis_File ovf;
	ov_callbacks ovc = {ov_read_func, ov_seek_func, ov_close_func, ov_tell_func};
	if (ov_open_callbacks(wave, &ovf, NULL, 0, ovc) < 0)
	{
		FS.r_close(wave);
		return false;
	}
	vorbis_info* ovi = ov_info(&ovf, -1);
	if (!ovi || ovi->channels < 1 || ovi->channels > 2)
	{
		ov_clear(&ovf);
		FS.r_close(wave);
		return false;
	}
	out.channels = ovi->channels;
	out.rate = (int)ovi->rate;

	// same OGG comment as CSoundRender_Source::LoadWave (only the fields the voice needs)
	out.base_volume = 1.f;
	out.max_ai_dist = 300.f;
	vorbis_comment* ovm = ov_comment(&ovf, -1);
	if (ovm && ovm->comments)
	{
		IReader F(ovm->user_comments[0], ovm->comment_lengths[0]);
		u32 vers = F.r_u32();
		if (vers == 0x0001)
		{
			float min_d = F.r_float();
			float max_d = F.r_float();
			(void)min_d;
			out.base_volume = 1.0f;
			F.r_u32();
			out.max_ai_dist = max_d;
		}
		else if (vers == 0x0002)
		{
			F.r_float();
			float max_d = F.r_float();
			out.base_volume = F.r_float();
			F.r_u32();
			out.max_ai_dist = max_d;
		}
		else if (vers == OGG_COMMENT_VERSION)
		{
			F.r_float();
			F.r_float();
			out.base_volume = F.r_float();
			F.r_u32();
			out.max_ai_dist = F.r_float();
		}
	}

	s64 total = ov_pcm_total(&ovf, -1);
	if (total <= 0)
	{
		ov_clear(&ovf);
		FS.r_close(wave);
		return false;
	}
	out.pcm.resize((size_t)total * out.channels);
	char* dest = (char*)out.pcm.data();
	long left = (long)(total * out.channels * 2), done = 0;
	int section = 0;
	while (done < left)
	{
		long ret = ov_read(&ovf, dest + done, left - done, 0, 2, 1, &section);
		if (ret <= 0) break;
		done += ret;
	}
	out.frames = (u32)(done / (2 * out.channels));
	ov_clear(&ovf);
	FS.r_close(wave);
	return out.frames > 0;
}

CSoundRender_CyclicVoice::SPcm* CSoundRender_CyclicVoice::get_pcm(const shared_str& name)
{
	if (!name.size()) return nullptr;
	xr_string key = name.c_str();
	std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)tolower(c); });
	auto it = m_pcm.find(key);
	if (it != m_pcm.end())
		return (it->second && it->second->ok) ? it->second : nullptr;

	if (m_pcm_bytes > PCM_CACHE_LIMIT && m_s.state == SSession::Idle)
	{
		for (int i = 0; i < m_pool; ++i)
			if (m_slot[i].src) stop_slot_now(i);
		free_pcm_all();
	}

	SPcm* p = new SPcm();
	p->ok = load_pcm(name.c_str(), *p);
	if (p->ok)
		m_pcm_bytes += p->pcm.size() * sizeof(s16);
	m_pcm[key] = p;
	if (p->ok)
		dbg("loaded %s: %u frames, %d ch, base_vol %.2f, ai_dist %.0f", name.c_str(), p->frames, p->channels, p->base_volume, p->max_ai_dist);
	return p->ok ? p : nullptr;
}

ALuint CSoundRender_CyclicVoice::whole_buffer(SPcm& p)
{
	if (!p.whole)
	{
		alGenBuffers(1, &p.whole);
		alBufferData(p.whole, p.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, p.pcm.data(),
		             (ALsizei)(p.frames * p.channels * sizeof(s16)), p.rate);
	}
	return p.whole;
}

ALuint CSoundRender_CyclicVoice::slice_buffer(SPcm& p, u32 first, u32 count)
{
	const u64 key = (u64(first) << 32) | u64(count);
	auto it = p.slices.find(key);
	if (it != p.slices.end()) return it->second;
	ALuint b = 0;
	alGenBuffers(1, &b);
	alBufferData(b, p.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, p.pcm.data() + size_t(first) * p.channels,
	             (ALsizei)(count * p.channels * sizeof(s16)), p.rate);
	p.slices[key] = b;
	return b;
}

// ---------------------------------------------------------------------------------------------------------------
// source slots
// ---------------------------------------------------------------------------------------------------------------

void CSoundRender_CyclicVoice::stop_slot_now(int si)
{
	SSlot& s = m_slot[si];
	alSourceStop(s.src);
	alSourcei(s.src, AL_BUFFER, 0);
	s.q.clear();
	s.pending = 0;
	s.env.active = false;
	s.in_use = false;
}

void CSoundRender_CyclicVoice::reset_slot(int si)
{
	SSlot& s = m_slot[si];
	alSourceStop(s.src);
	alSourcei(s.src, AL_BUFFER, 0);
	s.q.clear();
	s.pending = 0;
	s.env = SEnv();
	s.stop_when_env_done = false;
	s.pitch = 1.f;
	alSourcef(s.src, AL_PITCH, 1.f);
	s.release_after = 0;
}

void CSoundRender_CyclicVoice::release_slot(int si)
{
	if (si < 0) return;
	m_slot[si].in_use = false;
}

int CSoundRender_CyclicVoice::acquire_slot()
{
	int best = -1;
	// a free slot first
	for (int i = 0; i < m_pool; ++i)
		if (!m_slot[i].in_use && i != m_s.s_start && i != m_s.s_loop && i != m_s.s_end)
		{
			best = i;
			break;
		}
	// otherwise steal the oldest slot that is not a role of the current session
	if (best < 0)
	{
		u32 oldest = 0xffffffff;
		for (int i = 0; i < m_pool; ++i)
		{
			if (i == m_s.s_start || i == m_s.s_loop || i == m_s.s_end) continue;
			if (m_slot[i].stamp < oldest)
			{
				oldest = m_slot[i].stamp;
				best = i;
			}
		}
	}
	if (best < 0) best = 0;
	reset_slot(best);
	m_slot[best].in_use = true;
	m_slot[best].stamp = ++m_stamp;
	return best;
}

void CSoundRender_CyclicVoice::set_env(SSlot& s, bool fade_out, clk_t t0, float dur_s, int curve, float value_before)
{
	s.env.active = true;
	s.env.fade_out = fade_out;
	s.env.t0 = t0;
	s.env.dur = (clk_t)(std::max(dur_s, 0.001f) * NS);
	s.env.curve = curve;
	s.env.value_before = value_before;
}

void CSoundRender_CyclicVoice::apply_gain(SSlot& s, clk_t now)
{
	float m = 1.f;
	if (s.env.active)
	{
		if (now < s.env.t0)
			m = s.env.value_before;
		else
		{
			float p = clampf(float(double(now - s.env.t0) / double(s.env.dur)), 0.f, 1.f);
			if (s.env.fade_out)
				m = (s.env.curve == 1) ? cosf(p * HALF_PI) : (1.f - p);
			else
				m = (s.env.curve == 1) ? sinf(p * HALF_PI) : p;
		}
	}
	alSourcef(s.src, AL_GAIN, clampf(s.gain * m, 0.f, 1.f));
}

void CSoundRender_CyclicVoice::play_single(int si, SPcm* p, clk_t at, float pitch)
{
	SSlot& s = m_slot[si];
	s.gain = clampf(p->base_volume * effects_volume(), 0.f, 1.f);
	s.rate = p->rate;
	s.pitch = pitch;
	alSourcef(s.src, AL_PITCH, pitch);
	ALuint b = whole_buffer(*p);
	alSourcei(s.src, AL_BUFFER, (ALint)b);
	s.q.clear();
	s.pending = 0;
	s.play_clk = at;
	apply_gain(s, now_ns() - 1);
	m_alSourcePlayAtTimeSOFT(s.src, (ALint64SOFT)at);
}

// Queue one slice of a loop file. Block length = 60 / sample_rpm seconds; the last block runs to the end of the
// file (it keeps the full tail of the last mixed shot). Past the last block we wrap on the normal blocks.
void CSoundRender_CyclicVoice::queue_slice(int si, SPcm* p, float sample_rpm, int nshots, int idx)
{
	SSlot& s = m_slot[si];
	if (sample_rpm < 1.f) sample_rpm = 600.f;
	const u32 B = (u32)floorf(float(p->rate) * 60.f / sample_rpm + 0.5f);
	int n = nshots > 0 ? nshots : (int)(p->frames / (B ? B : 1));
	if (n < 1) n = 1;
	if (idx >= n)
		idx = (n > 1) ? (idx - n) % (n - 1) : 0;
	u32 first = (u32)idx * B;
	u32 last = (idx == n - 1) ? p->frames : std::min<u32>(p->frames, first + B);
	if (first >= p->frames)
	{
		first = 0;
		last = std::min<u32>(p->frames, B);
	}
	if (last <= first) return;
	ALuint b = slice_buffer(*p, first, last - first);
	alSourceQueueBuffers(s.src, 1, &b);
	s.q.push_back({b, last - first});
	s.pending += last - first;
	s.rate = p->rate;
}

void CSoundRender_CyclicVoice::reclaim(int si)
{
	SSlot& s = m_slot[si];
	ALint processed = 0;
	alGetSourcei(s.src, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0 && !s.q.empty())
	{
		ALuint b;
		alSourceUnqueueBuffers(s.src, 1, &b);
		s.pending -= s.q.front().samples;
		s.q.pop_front();
	}
}

// Time (ns) until the queue of slot `si` runs dry.
double CSoundRender_CyclicVoice::remaining_ns(int si, clk_t now)
{
	SSlot& s = m_slot[si];
	ALint off = 0;
	alGetSourcei(s.src, AL_SAMPLE_OFFSET, &off);
	double samples_left = double(s.pending) - double(off);
	if (samples_left < 0.0) samples_left = 0.0;
	double wait = (s.play_clk > now) ? double(s.play_clk - now) : 0.0;
	double pitch = std::max(0.1f, s.pitch);
	return wait + samples_left / (double(s.rate) * pitch) * NS;
}

void CSoundRender_CyclicVoice::ensure_playing(int si, clk_t now)
{
	SSlot& s = m_slot[si];
	ALint st = 0;
	alGetSourcei(s.src, AL_SOURCE_STATE, &st);
	if (st != AL_PLAYING && st != AL_PAUSED)
	{
		dbg("underrun on loop source (state %d) -> restart", st);
		reclaim(si); // a stopped source would otherwise replay the blocks it already played
		if (s.q.empty()) return;
		s.play_clk = now;
		m_alSourcePlayAtTimeSOFT(s.src, (ALint64SOFT)now);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// AI hearing
// ---------------------------------------------------------------------------------------------------------------

void CSoundRender_CyclicVoice::fire_ai(const SCyclicEvent& e, SPcm* p)
{
	if (!e.owner || !e.ai_type || !p) return;
	SoundRender->push_raw_event(e.owner, e.ai_type, p->max_ai_dist, 1.f);
}

// ---------------------------------------------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------------------------------------------

void CSoundRender_CyclicVoice::on_event(const SCyclicEvent& e)
{
	if (!m_ready) return;
	std::lock_guard<std::mutex> g(m_lock);
	switch (e.type)
	{
	case cyc_begin: on_begin(e); break;
	case cyc_shot: on_shot(e); break;
	case cyc_release: on_release(e); break;
	case cyc_abort: on_abort(); break;
	case cyc_jam: on_jam(e); break;
	}
}

void CSoundRender_CyclicVoice::on_begin(const SCyclicEvent& e)
{
	// A previous burst that is still looping is closed properly (its tail rings out on its own sources).
	if (m_s.state == SSession::Loop)
		schedule_end(m_s.last_e, now_ns(), false);
	m_s = SSession();
	m_s.last_e = e;

	const clk_t now = now_ns();
	m_s.state = SSession::Start;
	m_s.owner = e.owner;
	m_s.ai_type = e.ai_type;
	m_s.shots = 1;
	m_s.last_shot_clk = now;
	m_s.cycle_ns = float(NS * 60.0 / std::max(1.f, e.real_rpm));

	SPcm* p = get_pcm(e.start_name);
	fire_ai(e, p ? p : get_pcm(e.loop_name));
	if (!p) return;
	const clk_t lat = latency_ns();
	m_s.lat_ns = lat;
	int si = acquire_slot();
	m_s.s_start = si;
	play_single(si, p, now + lat, 1.f);
	dbg("begin: start=%s rpm=%.0f latency=%.1fms (frame %.1fms)", e.start_name.c_str(), e.real_rpm, double(lat) / 1.0e6, m_frame_ms);
	if (e.last_shot)
		m_s.state = SSession::Idle; // single shot: the sample carries its own tail
}

void CSoundRender_CyclicVoice::on_shot(const SCyclicEvent& e)
{
	if (m_s.state == SSession::Idle || m_s.state == SSession::Ending)
	{
		// The burst was closed (safety timeout during a long freeze) but the weapon keeps firing: pick the loop up again
		// right away, at the block that matches the shot number, instead of staying silent until the next shot.
		on_begin(e);
		if (e.shot_no >= 2 && e.loop_name.size() && m_s.state == SSession::Start)
		{
			m_s.shots = e.shot_no;
			start_loop(e, now_ns(), e.shot_no - 2);
		}
		return;
	}
	const clk_t now = now_ns();
	m_s.shots++;
	if (m_s.last_shot_clk && m_s.shots >= 3)
	{
		// median of the last 5 intervals, then a slow average: one lag spike never moves the pitch
		double ivl = double(now - m_s.last_shot_clk);
		m_s.ivl_hist[m_s.ivl_count % 5] = ivl;
		m_s.ivl_count++;
		const int n = std::min(m_s.ivl_count, 5);
		double tmp[5];
		for (int i = 0; i < n; ++i) tmp[i] = m_s.ivl_hist[i];
		std::sort(tmp, tmp + n);
		const double med = tmp[n / 2];
		m_s.ivl_ema_ns = (m_s.ivl_ema_ns <= 0.0) ? med : (0.8 * m_s.ivl_ema_ns + 0.2 * med);
	}
	m_s.last_shot_clk = now;
	m_s.cycle_ns = float(NS * 60.0 / std::max(1.f, e.real_rpm));
	m_s.owner = e.owner;
	m_s.ai_type = e.ai_type;
	m_s.last_e = e;

	if (!e.loop_name.size())
	{
		fire_ai(e, get_pcm(e.start_name));
		if (e.last_shot) m_s.state = SSession::Idle;
		return;
	}
	fire_ai(e, get_pcm(e.loop_name));

	if (!m_s.loop_started)
		start_loop(e, now);
	else
		append_block(e, now);

	if (e.last_shot && m_s.state == SSession::Loop)
		schedule_end(e, now, false);
}

void CSoundRender_CyclicVoice::start_loop(const SCyclicEvent& e, clk_t now, int first_block)
{
	SPcm* p = get_pcm(e.loop_name);
	if (!p) return;
	const clk_t lat = m_s.lat_ns ? m_s.lat_ns : latency_ns();
	const clk_t t_play = now + lat + (clk_t)(e.loop_delay_s * NS);

	int si = acquire_slot();
	m_s.s_loop = si;
	SSlot& s = m_slot[si];
	s.gain = clampf(p->base_volume * effects_volume(), 0.f, 1.f);
	m_s.loop_pcm = p;
	m_s.loop_name = e.loop_name;
	m_s.loop_sample_rpm = e.loop_sample_rpm;
	m_s.loop_shots = e.loop_shots;
	m_s.base_pitch = clampf(e.real_rpm / std::max(1.f, e.loop_sample_rpm), 0.5f, 2.f);
	s.pitch = m_s.base_pitch;
	alSourcef(s.src, AL_PITCH, s.pitch);
	queue_slice(si, p, e.loop_sample_rpm, e.loop_shots, first_block);
	s.play_clk = t_play;
	apply_gain(s, now);
	m_alSourcePlayAtTimeSOFT(s.src, (ALint64SOFT)t_play);
	m_s.loop_started = true;
	m_s.state = SSession::Loop;
	m_s.loop_target_ns = lat + (clk_t)(e.loop_delay_s * NS);

	if (e.start_cutoff_on_loop && m_s.s_start >= 0)
	{
		m_s.cut_pending = true;
		m_s.cut_clk = t_play;
		m_s.cut_cfg = e;
	}
	dbg("loop start: %s pitch %.3f (real %.0f / sample %.0f rpm) at +%.1fms", e.loop_name.c_str(), s.pitch,
	    e.real_rpm, e.loop_sample_rpm, double(t_play - now) / 1.0e6);
}

void CSoundRender_CyclicVoice::append_block(const SCyclicEvent& e, clk_t now)
{
	const int si = m_s.s_loop;
	if (si < 0) return;
	SSlot& s = m_slot[si];

	// A source that ran out of blocks (lag spike, slow frame) is rebuilt from scratch instead of asking OpenAL which
	// buffers are "processed": on a stopped source that answer is unreliable and the block we queue next could be
	// thrown away with the old ones, leaving the loop silent.
	auto is_dry = [&]() {
		ALint st = 0;
		alGetSourcei(s.src, AL_SOURCE_STATE, &st);
		return !m_paused && st != AL_PLAYING && st != AL_PAUSED;
	};
	auto rebuild = [&]() {
		alSourceStop(s.src);
		alSourcei(s.src, AL_BUFFER, 0);
		s.q.clear();
		s.pending = 0;
	};

	bool dry = is_dry();
	if (dry)
	{
		dbg("underrun on loop source -> rebuild and restart at the shot");
		rebuild();
	}
	else
		reclaim(si);
	const double rem_before = dry ? 0.0 : remaining_ns(si, now);

	// start of a transition sample (indoor <-> outdoor): used for trans_shots blocks, then back to the loop
	if (e.trans_name.size() && m_s.trans_left <= 0)
	{
		m_s.trans_pcm = get_pcm(e.trans_name);
		m_s.trans_left = e.trans_shots > 0 ? e.trans_shots : 3;
		m_s.trans_idx = 0;
		m_s.trans_sample_rpm = e.trans_sample_rpm;
		m_s.trans_shots = e.trans_shots;
		dbg("transition sample %s for %d blocks", e.trans_name.c_str(), m_s.trans_left);
	}

	// which block, decided once (so a retry below queues the very same one)
	SPcm* blk_pcm = nullptr;
	float blk_rpm = 600.f;
	int blk_shots = 0, blk_idx = 0;
	if (m_s.trans_left > 0 && m_s.trans_pcm)
	{
		blk_pcm = m_s.trans_pcm;
		blk_rpm = m_s.trans_sample_rpm;
		blk_shots = m_s.trans_shots;
		blk_idx = m_s.trans_idx++;
		m_s.trans_left--;
	}
	else
	{
		if (e.loop_name != m_s.loop_name)
		{
			SPcm* p = get_pcm(e.loop_name);
			if (p)
			{
				m_s.loop_pcm = p;
				m_s.loop_name = e.loop_name;
				m_s.loop_sample_rpm = e.loop_sample_rpm;
				m_s.loop_shots = e.loop_shots;
				dbg("loop swap -> %s at block %d", e.loop_name.c_str(), m_s.shots - 2);
			}
		}
		blk_pcm = m_s.loop_pcm;
		blk_rpm = m_s.loop_sample_rpm;
		blk_shots = m_s.loop_shots;
		blk_idx = m_s.shots - 2;
	}
	if (!blk_pcm) return;

	queue_slice(si, blk_pcm, blk_rpm, blk_shots, blk_idx);

	// The mixer may have emptied the queue between the check above and the queueing: look again and, if so, restart
	if (!dry && is_dry())
	{
		dbg("underrun during queueing -> rebuild and restart at the shot");
		rebuild();
		queue_slice(si, blk_pcm, blk_rpm, blk_shots, blk_idx);
		dry = true;
	}
	if (dry)
	{
		s.play_clk = now;
		apply_gain(s, now);
		m_alSourcePlayAtTimeSOFT(s.src, (ALint64SOFT)now);
	}
	servo(e, now, rem_before);
}

// Keep the audio exactly as long as the real shot interval: pitch = nominal block length / measured interval, plus
// a small correction that keeps the amount of audio already queued at `loop_target_ns` (see doc 08, section 4.4).
void CSoundRender_CyclicVoice::servo(const SCyclicEvent& e, clk_t now, double remaining_before_ns)
{
	(void)now;
	if (m_s.s_loop < 0 || m_s.shots < 4 || m_s.ivl_ema_ns <= 0.0) return;
	SSlot& s = m_slot[m_s.s_loop];
	const double block_ns = NS * 60.0 / std::max(1.f, m_s.loop_sample_rpm);
	double p = block_ns / m_s.ivl_ema_ns;
	double err = (remaining_before_ns - double(m_s.loop_target_ns)) / m_s.ivl_ema_ns;
	p *= 1.0 + clampf(float(0.1 * err), -0.01f, 0.01f);
	const double lo = m_s.base_pitch * 0.9, hi = m_s.base_pitch * 1.1;
	p = std::max(lo, std::min(hi, p));
	p = std::max(0.5, std::min(2.0, p));
	if (std::fabs(p - s.pitch) > 0.0005)
	{
		s.pitch = (float)p;
		alSourcef(s.src, AL_PITCH, s.pitch);
	}
	dbg("shot %d: interval %.2fms pitch %.4f queued-ahead %.1fms (target %.1fms)", m_s.shots, m_s.ivl_ema_ns / 1.0e6,
	    s.pitch, remaining_before_ns / 1.0e6, double(m_s.loop_target_ns) / 1.0e6);
}

void CSoundRender_CyclicVoice::schedule_end(const SCyclicEvent& e, clk_t now, bool immediate)
{
	if (m_s.state != SSession::Loop || m_s.s_loop < 0) return;
	SSlot& loop = m_slot[m_s.s_loop];
	reclaim(m_s.s_loop);

	double rem = immediate ? 0.0 : remaining_ns(m_s.s_loop, now);
	clk_t t_dry = now + (clk_t)rem;
	clk_t t_end = t_dry - (clk_t)(e.end_margin_s * NS);
	if (t_end < now) t_end = now;

	SPcm* pe = get_pcm(e.end_name);
	if (pe)
	{
		int si = acquire_slot();
		m_s.s_end = si;
		SSlot& es = m_slot[si];
		if (e.end_fadein_enable)
		{
			float dur = std::min(e.end_fadein_s, 0.9f * m_s.cycle_ns / float(NS));
			set_env(es, false, t_end, dur, e.end_fadein_curve, 0.f);
		}
		play_single(si, pe, t_end, 1.f);
	}

	if (immediate || e.loop_fadeout_enable)
	{
		float dur = immediate ? 0.02f : std::min(e.loop_fadeout_s, 0.9f * m_s.cycle_ns / float(NS));
		set_env(loop, true, t_end, dur, immediate ? 0 : e.loop_fadeout_curve, 1.f);
		loop.stop_when_env_done = true;
	}
	loop.release_after = t_dry + (clk_t)(0.05 * NS);

	m_s.state = SSession::Ending;
	m_s.cleanup_clk = t_dry + (clk_t)(0.1 * NS);
	dbg("end: loop dry in %.1fms, end sample %s at +%.1fms", rem / 1.0e6, e.end_name.c_str(), double(t_end - now) / 1.0e6);
}

void CSoundRender_CyclicVoice::on_release(const SCyclicEvent& e)
{
	if (m_s.state == SSession::Loop)
		schedule_end(e, now_ns(), false);
	else if (m_s.state == SSession::Start)
		m_s.state = SSession::Idle; // only the start sample was played; its tail is baked in
}

void CSoundRender_CyclicVoice::on_jam(const SCyclicEvent& e)
{
	if (m_s.state == SSession::Loop)
		schedule_end(e, now_ns(), true);
	else if (m_s.state == SSession::Start)
		m_s.state = SSession::Idle;
}

void CSoundRender_CyclicVoice::on_abort()
{
	const clk_t now = now_ns();
	const int roles[3] = {m_s.s_start, m_s.s_loop, m_s.s_end};
	for (int i = 0; i < 3; ++i)
	{
		if (roles[i] < 0) continue;
		SSlot& s = m_slot[roles[i]];
		set_env(s, true, now, 0.008f, 0, 1.f);
		s.stop_when_env_done = true;
	}
	m_s.state = SSession::Idle;
	dbg("abort");
}

// ---------------------------------------------------------------------------------------------------------------
// frame update (sound task)
// ---------------------------------------------------------------------------------------------------------------

void CSoundRender_CyclicVoice::update(float dt)
{
	if (!m_ready) return;
	std::lock_guard<std::mutex> g(m_lock);
	if (dt > 0.f && dt < 0.5f)
	{
		// a lag spike must not inflate the latency for the rest of the burst: cap each sample at 3x the average
		const float f = std::min(dt * 1000.f, m_frame_ms * 3.f);
		m_frame_ms = m_frame_ms * 0.95f + f * 0.05f;
	}
	if (m_paused) return;
	const clk_t now = now_ns();

	if (!psSoundCyclicNative && m_s.state != SSession::Idle)
		on_abort();

	// start sample cut when the loop begins
	if (m_s.cut_pending && now >= m_s.cut_clk)
	{
		m_s.cut_pending = false;
		if (m_s.s_start >= 0)
		{
			SSlot& st = m_slot[m_s.s_start];
			const SCyclicEvent& c = m_s.cut_cfg;
			if (c.start_cutoff_fade_enable)
			{
				float dur = std::min(c.start_cutoff_fade_s, 0.9f * m_s.cycle_ns / float(NS));
				set_env(st, true, now, dur, c.start_cutoff_fade_curve, 1.f);
				st.stop_when_env_done = true;
			}
			else if (c.start_cutoff_soft)
				st.gain = 0.f;
			else
				stop_slot_now(m_s.s_start);
		}
	}

	// safety: a loop that stopped receiving shots without any release (weapon vanished): close it
	if (m_s.state == SSession::Loop && m_s.last_shot_clk && double(now - m_s.last_shot_clk) > 3.0 * double(m_s.cycle_ns) + 150.0e6)
	{
		schedule_end(m_s.last_e, now, false);
	}

	if (m_s.state == SSession::Ending && now >= m_s.cleanup_clk)
	{
		m_s.state = SSession::Idle;
		m_s.s_start = m_s.s_loop = m_s.s_end = -1;
	}

	for (int i = 0; i < m_pool; ++i)
	{
		SSlot& s = m_slot[i];
		if (!s.in_use) continue;
		ALint st = 0;
		alGetSourcei(s.src, AL_SOURCE_STATE, &st);
		if (s.env.active)
		{
			apply_gain(s, now);
			if (s.stop_when_env_done && now >= s.env.t0 + s.env.dur)
			{
				bool role = (i == m_s.s_start || i == m_s.s_loop || i == m_s.s_end);
				stop_slot_now(i);
				if (role && m_s.state == SSession::Idle) { /* roles are cleared below */ }
				continue;
			}
		}
		else if (st == AL_PLAYING)
			apply_gain(s, now);

		const bool role = (i == m_s.s_start || i == m_s.s_loop || i == m_s.s_end);
		if (!role && st != AL_PLAYING && st != AL_PAUSED && now > s.play_clk + (clk_t)(0.05 * NS))
		{
			reclaim(i);
			s.in_use = false;
		}
		else if (role && st == AL_STOPPED && m_s.state != SSession::Loop && now > s.play_clk + (clk_t)(0.05 * NS))
		{
			reclaim(i);
		}
		if (!s.env.active && s.q.size() > 1 && i != m_s.s_loop)
			reclaim(i);
	}

	if (m_s.state == SSession::Idle)
		m_s.s_start = m_s.s_loop = m_s.s_end = -1;
}

void CSoundRender_CyclicVoice::pause(bool val)
{
	if (!m_ready) return;
	std::lock_guard<std::mutex> g(m_lock);
	if (val == m_paused) return;
	m_paused = val;
	for (int i = 0; i < m_pool; ++i)
	{
		SSlot& s = m_slot[i];
		if (!s.in_use) continue;
		ALint st = 0;
		alGetSourcei(s.src, AL_SOURCE_STATE, &st);
		if (val && st == AL_PLAYING)
			alSourcePause(s.src);
		else if (!val && st == AL_PAUSED)
			alSourcePlay(s.src);
	}
}

void CSoundRender_CyclicVoice::stop_all()
{
	if (!m_ready) return;
	std::lock_guard<std::mutex> g(m_lock);
	for (int i = 0; i < m_pool; ++i)
		if (m_slot[i].src) stop_slot_now(i);
	m_s = SSession();
}
