#include "stdafx.h"
#pragma hdrstop

#include <algorithm>

#include "cl_intersect.h"
#include "SoundRender_Core.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_TargetA.h"
#include "SoundRender_Source.h"
#include "SoundRender_CoreA.h"

// Phase 0 occlusion instrumentation (29/09): tiny RAII scope timer, same idea as the engine's other
// QPC-based timers (CTimer/FTimer.h). Adds its elapsed ticks to 'acc' on destruction.
namespace
{
struct occ_scope_timer
{
	u64 t0;
	u64& acc;
	occ_scope_timer(u64& a) : t0(CPU::QPC()), acc(a) {}
	~occ_scope_timer() { acc += CPU::QPC() - t0; }
};
} // namespace

CSoundRender_Emitter* CSoundRender_Core::i_play(ref_sound* S, BOOL _loop, float delay)
{
	VERIFY(S->_p->feedback==0);
	CSoundRender_Emitter* E = xr_new<CSoundRender_Emitter>();
	S->_p->feedback = E;
	E->start(S, _loop, delay);
	s_emitters.push_back(E);
	return E;
}

void CSoundRender_Core::update(const Fvector& P, const Fvector& D, const Fvector& N)
{
	u32 it;

	if (0 == bReady) return;

	if (bPendingDeviceListRefresh)
	{
		bPendingDeviceListRefresh = FALSE;
		refresh_devices();
	}
	if (bPendingDefaultDeviceSwitch)
	{
		bPendingDefaultDeviceSwitch = FALSE;
		default_device_changed();
	}

	bLocked = TRUE;
	float new_tm = Timer.GetElapsed_sec();
	fTimer_Delta = new_tm - fTimer_Value;
	//.	float dt					= float(Timer_Delta)/1000.f;
	float dt_sec = fTimer_Delta;
	fTimer_Value = new_tm;

	// Phase 0 occlusion instrumentation (29/09): close out the previous frame's occlusion counters
	// (get_occlusion/get_occlusion_to/update_culling all accumulate into m_occ_cur, below, over the
	// rest of THIS update() call and via CSoundRender_Emitter::update_culling) and start a fresh one.
	// m_occ_last therefore always reflects one fully-completed frame -- stable for statistic() to read
	// at any point until the next update() call. Purely additive: nothing here changes occlusion
	// itself, only whether its cost gets measured.
	m_occ_last = m_occ_cur;
	m_occ_cur.reset();
	m_occ_budget_used = 0;
	if (psSoundOcclusionStats)
	{
		m_occ_window.ticks += m_occ_last.ticks;
		m_occ_window.ticks_ai += m_occ_last.ticks_ai;
		m_occ_window.calls += m_occ_last.calls;
		m_occ_window.rays += m_occ_last.rays;
		m_occ_window.blocked += m_occ_last.blocked;
		m_occ_window.calls_ai += m_occ_last.calls_ai;
		m_occ_window.emitters_3d += m_occ_last.emitters_3d;
		m_occ_window_max_ticks = _max(m_occ_window_max_ticks, m_occ_last.ticks);
		m_occ_window_frames++;
		m_occ_window_time += fTimer_Delta;
		if (m_occ_window_time >= 5.f)
		{
			const double to_ms = double(CPU::qpc_freq) > 0.0 ? 1000.0 / double(CPU::qpc_freq) : 0.0;
			const float f = float(m_occ_window_frames);
			Msg("* [snd_occ] mode=%d  avg %.3fms/frame  max %.3fms  calls/s %.0f  rays/s %.0f  blocked %.0f%%  "
				"3D emitters/frame %.1f  |  AI %.3fms/frame (%.0f calls/s)",
				psSoundOcclusionMode,
				float(m_occ_window.ticks * to_ms) / f, float(m_occ_window_max_ticks * to_ms),
				m_occ_window.calls / m_occ_window_time, m_occ_window.rays / m_occ_window_time,
				m_occ_window.calls ? 100.f * m_occ_window.blocked / m_occ_window.calls : 0.f,
				m_occ_window.emitters_3d / f,
				float(m_occ_window.ticks_ai * to_ms) / f, m_occ_window.calls_ai / m_occ_window_time);
			m_occ_window.reset();
			m_occ_window_frames = 0;
			m_occ_window_time = 0.f;
			m_occ_window_max_ticks = 0;
		}
	}

	s_emitters_u ++;

	// Firstly update emitters, which are now being rendered
	//Msg	("! update: r-emitters");
	for (it = 0; it < s_targets.size(); it++)
	{
		CSoundRender_Target* T = s_targets[it];
		CSoundRender_Emitter* E = T->get_emitter();
		if (E)
		{
			E->update(dt_sec);
			E->marker = s_emitters_u;
			E = T->get_emitter(); // update can stop itself
			if (E) T->priority = E->priority();
			else T->priority = -1;
		}
		else
		{
			T->priority = -1;
		}
	}

	// Update emmitters
	//Msg	("! update: emitters");
	for (it = 0; it < s_emitters.size(); it++)
	{
		CSoundRender_Emitter* pEmitter = s_emitters[it];
		if (pEmitter->marker != s_emitters_u)
		{
			pEmitter->update(dt_sec);
			pEmitter->marker = s_emitters_u;
		}
		if (!pEmitter->isPlaying())
		{
			// Stopped
			xr_delete(pEmitter);
			s_emitters.erase(s_emitters.begin() + it);
			it--;
		}
	}

	// Get currently rendering emitters
	//Msg	("! update: targets");
	s_targets_defer.clear();
	s_targets_pu ++;
	// u32 PU				= s_targets_pu%s_targets.size();
	for (it = 0; it < s_targets.size(); it++)
	{
		CSoundRender_Target* T = s_targets[it];
		if (T->get_emitter())
		{
			// Has emmitter, maybe just not started rendering
			if (T->get_Rendering())
			{
				/*if	(PU == it)*/
				T->fill_parameters();
				T->update();
			}
			else
				s_targets_defer.push_back(T);
		}
	}

	// Commit parameters from pending targets
	if (!s_targets_defer.empty())
	{
		//Msg	("! update: start render - commit");
		s_targets_defer.erase(std::unique(s_targets_defer.begin(), s_targets_defer.end()), s_targets_defer.end());
		for (it = 0; it < s_targets_defer.size(); it++)
			s_targets_defer[it]->fill_parameters();
	}

	// update EFX
	if (m_is_supported)
	{
		if (bListenerMoved)
		{
			bListenerMoved = FALSE;
			e_target_ptr = get_environment(P);
			if (!e_target_ptr)
				e_target_ptr = &e_identity;
		}

		// demonized: Interpolate from e_current to 95% of e_target in close to exact time
		constexpr float percent = 0.95f;
		float alpha = 1.0f - std::exp(std::log(1.0f - percent) * dt_sec / snd_efx_environment_change_time);
		clamp(alpha, 0.f, 1.f);
		//Msg("interpolating from e_current to e_target %.2f", std::min(e_current.Reverb, e_target_ptr->Reverb) / std::max(e_current.Reverb, e_target_ptr->Reverb));
		e_current.lerp(e_current, *e_target_ptr, alpha);

		set_listener(e_current);
		commit();
	}

	// update listener
	update_listener(P, D, N, dt_sec);

	// Start rendering of pending targets
	if (!s_targets_defer.empty())
	{
		CSoundRender_CoreA* Core = (CSoundRender_CoreA*)this;
		//Msg	("! update: start render");
		for (it = 0; it < s_targets_defer.size(); it++)
		{
			CSoundRender_TargetA* Ptr = (CSoundRender_TargetA*)s_targets_defer[it];
			if (m_is_supported)
				Ptr->SetSlot(Core->slot);
			Ptr->render();
		}
	}

	// Cyclic gunfire voice (doc 08): envelopes, start-sample cut, cleanup
	cyclic_update(dt_sec);

	// Events
	update_events();

	bLocked = FALSE;
}

static u32 g_saved_event_count = 0;

void CSoundRender_Core::update_events()
{
	PROF_EVENT("Sound: Update Events");
	g_saved_event_count = s_events.size();
	for (u32 it = 0; it < s_events.size(); it++)
	{
		event& E = s_events[it];
		Handler(E.first, E.second);
	}
	s_events.clear_not_free();

	// AI-hearing events from the cyclic voice (doc 08)
	{
		std::vector<SRawAIEvent> raw;
		{
			std::lock_guard<std::mutex> g(s_raw_lock);
			raw.swap(s_raw_events);
		}
		if (HandlerRaw)
			for (const SRawAIEvent& ev : raw)
				HandlerRaw(ev.who, ev.type, ev.pos, ev.max_ai, ev.vol);
	}
}

void CSoundRender_Core::statistic(CSound_stats* dest, CSound_stats_ext* ext)
{
	if (dest)
	{
		dest->_rendered = 0;
		for (u32 it = 0; it < s_targets.size(); it++)
		{
			CSoundRender_Target* T = s_targets[it];
			if (T->get_emitter() && T->get_Rendering()) dest->_rendered++;
		}
		dest->_simulated = s_emitters.size();
		dest->_cache_hits = cache._stat_hit;
		dest->_cache_misses = cache._stat_miss;
		dest->_events = g_saved_event_count;
		cache.stats_clear();

		// Phase 0 occlusion instrumentation: last fully-completed frame's counters (see the
		// m_occ_cur/m_occ_last handoff at the top of update()). All zero if snd_occlusion_stats is off.
		const double to_ms = double(CPU::qpc_freq) > 0.0 ? 1000.0 / double(CPU::qpc_freq) : 0.0;
		dest->_occ_ms = float(m_occ_last.ticks * to_ms);
		dest->_occ_calls = m_occ_last.calls;
		dest->_occ_rays = m_occ_last.rays;
		dest->_occ_blocked = m_occ_last.blocked;
		dest->_occ_ai_ms = float(m_occ_last.ticks_ai * to_ms);
		dest->_occ_ai_calls = m_occ_last.calls_ai;
		dest->_emitters_3d = m_occ_last.emitters_3d;
	}
	if (ext)
	{
		for (u32 it = 0; it < s_emitters.size(); it++)
		{
			CSoundRender_Emitter* _E = s_emitters[it];
			CSound_stats_ext::SItem _I;
			_I._3D = !_E->b2D;
			_I._rendered = !!_E->target;
			_I.params = _E->p_source;
			_I.volume = _E->smooth_volume;
			if (_E->owner_data)
			{
				_I.name = _E->source()->fname;
				_I.game_object = _E->owner_data->g_object;
				_I.game_type = _E->owner_data->g_type;
				_I.type = _E->owner_data->s_type;
			}
			else
			{
				_I.game_object = 0;
				_I.game_type = 0;
				_I.type = st_Effect;
			}
			ext->append(_I);
		}
	}
}


float CSoundRender_Core::get_occlusion_to(const Fvector& hear_pt, const Fvector& snd_pt, float dispersion)
{
	occ_scope_timer _t(m_occ_cur.ticks_ai);
	m_occ_cur.calls_ai++;

	float occ_value = 1.f;

	if (0 != geom_SOM)
	{
		// Calculate RAY params
		Fvector pos, dir;
		pos.random_dir();
		pos.mul(dispersion);
		pos.add(snd_pt);
		dir.sub(pos, hear_pt);
		float range = dir.magnitude();
		dir.div(range);

#ifdef _EDITOR
		ETOOLS::ray_options		(CDB::OPT_CULL);
		ETOOLS::ray_query		(geom_SOM,hear_pt,dir,range);
		u32 r_cnt				= ETOOLS::r_count();
		CDB::RESULT*	_B 		= ETOOLS::r_begin();
#else
		geom_DB.ray_options(CDB::OPT_CULL);
		geom_DB.ray_query(geom_SOM, hear_pt, dir, range);
		u32 r_cnt = geom_DB.r_count();
		CDB::RESULT* _B = geom_DB.r_begin();
#endif
		if (0 != r_cnt)
		{
			for (u32 k = 0; k < r_cnt; k++)
			{
				CDB::RESULT* R = _B + k;
				occ_value *= *(float*)&R->dummy;
			}
		}
	}
	return occ_value;
}

float CSoundRender_Core::get_occlusion(Fvector& P, float R, Fvector* occ)
{
	occ_scope_timer _t(m_occ_cur.ticks);
	m_occ_cur.calls++;

	float occ_value = 1.f;

	// Calculate RAY params
	Fvector base = listener_position();
	Fvector pos, dir;
	float range;
	pos.random_dir();
	pos.mul(R);
	pos.add(P);
	dir.sub(pos, base);
	range = dir.magnitude();
	dir.div(range);

	if (0 != geom_MODEL)
	{
		bool bNeedFullTest = true;
		// 1. Check cached polygon
		float _u, _v, _range;
		if (CDB::TestRayTri(base, dir, occ, _u, _v, _range, true))
			if (_range > 0 && _range < range)
			{
				occ_value = psSoundOcclusionScale;
				bNeedFullTest = false;
			}
		// 2. Polygon doesn't picked up - real database query
		if (bNeedFullTest)
		{
			m_occ_cur.rays++;
#ifdef _EDITOR
			ETOOLS::ray_options		(CDB::OPT_ONLYNEAREST);
			ETOOLS::ray_query		(geom_MODEL,base,dir,range);
			if (0!=ETOOLS::r_count()){
				// cache polygon
				const CDB::RESULT*	R = ETOOLS::r_begin			();
#else
			geom_DB.ray_options(CDB::OPT_ONLYNEAREST);
			geom_DB.ray_query(geom_MODEL, base, dir, range);
			if (0 != geom_DB.r_count())
			{
				// cache polygon
				const CDB::RESULT* R = geom_DB.r_begin();
#endif
				const CDB::TRI& T = geom_MODEL->get_tris()[R->id];
				const Fvector* V = geom_MODEL->get_verts();
				occ[0].set(V[T.verts[0]]);
				occ[1].set(V[T.verts[1]]);
				occ[2].set(V[T.verts[2]]);
				occ_value = psSoundOcclusionScale;
			}
		}
	}
	if (0 != geom_SOM)
	{
		m_occ_cur.rays++;
#ifdef _EDITOR
		ETOOLS::ray_options		(CDB::OPT_CULL);
		ETOOLS::ray_query		(geom_SOM,base,dir,range);
		u32 r_cnt				= ETOOLS::r_count();
        CDB::RESULT*	_B 		= ETOOLS::r_begin();
#else
		geom_DB.ray_options(CDB::OPT_CULL);
		geom_DB.ray_query(geom_SOM, base, dir, range);
		u32 r_cnt = geom_DB.r_count();
		CDB::RESULT* _B = geom_DB.r_begin();
#endif
		if (0 != r_cnt)
		{
			for (u32 k = 0; k < r_cnt; k++)
			{
				CDB::RESULT* R = _B + k;
				occ_value *= *(float*)&R->dummy;
			}
		}
	}
	if (occ_value < 1.f)
		m_occ_cur.blocked++;
	return occ_value;
}

//-----------------------------------------------------------------------------
// Phase 1/2 occlusion rework (29/09), "snd_occlusion_mode 1". See docs/ENGINE_CHANGES_SOUND_OCCLUSION.md.
// The mode-0 path above (get_occlusion/get_occlusion_to) is completely untouched by everything below.
//-----------------------------------------------------------------------------

void CSoundRender_Core::set_occlusion_materials(const SSoundOcclusionMaterial* table, u32 count)
{
	m_occ_materials.assign(table, table + count);
}

void CSoundRender_Core::set_occlusion_limits(float max_loss_db, float max_hf_loss_db, float max_thickness_m,
	float max_loss_db_indoor, float max_hf_loss_db_indoor)
{
	m_occ_max_loss_db_indoor = _max(max_loss_db_indoor, 0.f);
	m_occ_max_hf_loss_db_indoor = _max(max_hf_loss_db_indoor, 0.f);
	m_occ_max_loss_db = _max(max_loss_db, 0.f);
	m_occ_max_hf_loss_db = _max(max_hf_loss_db, 0.f);
	m_occ_max_thickness_m = _max(max_thickness_m, 0.f);
}

// One "all hits, sorted, material-priced" ray. Every crossed surface contributes its class's flat
// loss_db once; loss_db_per_m only applies when the VERY NEXT hit (by distance) shares the same
// material -- a real measured entry+exit pair -- never guessed for a lone/unpaired hit (game collision
// meshes are frequently single-sided, so a clean pair isn't always available).
void CSoundRender_Core::occ_trace_losses(const Fvector& from, const Fvector& to, float& out_loss_db, float& out_hf_db, u32& rays,
	float* out_raw_loss_db)
{
	out_loss_db = 0.f;
	out_hf_db = 0.f;
	if (out_raw_loss_db)
		*out_raw_loss_db = 0.f;
	if (!geom_MODEL)
		return;

	Fvector dir;
	dir.sub(to, from);
	float range = dir.magnitude();
	if (range < 0.05f)
		return;
	dir.div(range);

	rays++;
	geom_DB.ray_options(0); // neither CULL nor ONLYFIRST nor ONLYNEAREST -> every crossed triangle
	geom_DB.ray_query(geom_MODEL, from, dir, range);
	const int r_cnt = geom_DB.r_count();
	if (r_cnt <= 0)
		return;

	static xr_vector<CDB::RESULT*> hits; // static: reused scratch buffer, this never recurses/threads
	hits.clear();
	hits.reserve(r_cnt);
	CDB::RESULT* B = geom_DB.r_begin();
	for (int i = 0; i < r_cnt; ++i)
		hits.push_back(B + i);
	std::sort(hits.begin(), hits.end(), [](const CDB::RESULT* a, const CDB::RESULT* b) { return a->range < b->range; });

	for (size_t i = 0; i < hits.size(); ++i)
	{
		const u32 mat_id = hits[i]->material;
		const SSoundOcclusionMaterial& mat = mat_id < m_occ_materials.size() ? m_occ_materials[mat_id] : m_occ_default_material;
		if (mat.ignore)
			continue;

		out_loss_db += mat.loss_db;
		out_hf_db += mat.hf_loss_db;

		if (i + 1 < hits.size() && hits[i + 1]->material == mat_id)
		{
			const float thickness = _min(hits[i + 1]->range - hits[i]->range, m_occ_max_thickness_m);
			if (thickness > 0.f)
				out_loss_db += mat.loss_db_per_m * thickness;
			++i; // consumed as this pair's exit face -- don't also count it as its own surface
		}
	}

	// Caps interpolate between the outdoor and indoor values, both from sound_occlusion.ltx (02/10).
	const float f = occ_indoor_f();
	const float cap_loss = m_occ_max_loss_db + (m_occ_max_loss_db_indoor - m_occ_max_loss_db) * f;
	const float cap_hf = m_occ_max_hf_loss_db + (m_occ_max_hf_loss_db_indoor - m_occ_max_hf_loss_db) * f;

	if (out_raw_loss_db)
		*out_raw_loss_db = out_loss_db; // pre-cap sum, for the debug log: how far past the cap this ray would go
	out_loss_db = _min(out_loss_db, cap_loss);
	out_hf_db = _min(out_hf_db, cap_hf);
}

// Phase 2 diffraction: only called when the direct path (occ_trace_losses, above) is already
// significantly blocked. Tests candidate over/around paths -- above the obstacle at three heights, and
// to either side -- keeps the shortest CLEAR one (both legs pass an OPT_ONLYFIRST test), and converts
// its extra path length into a frequency-dependent loss via the Maekawa diffraction approximation.
// Returns blocked=true (silence from this function) only if every candidate is itself blocked.
CSoundRender_Core::SSoundOcclusionResult CSoundRender_Core::occ_try_diffraction(const Fvector& L, const Fvector& S, float direct_dist)
{
	SSoundOcclusionResult res;
	res.blocked = true;
	res.gain = 0.f;
	res.gain_hf = 0.f;
	if (!geom_MODEL || direct_dist < 0.5f)
		return res;

	Fvector dir;
	dir.sub(S, L);
	dir.div(direct_dist);
	Fvector up, right;
	Fvector::generate_orthonormal_basis(dir, up, right);

	// Nearest blocker from each end -- anchors the detour candidates around where the obstruction
	// actually is, rather than blindly around the geometric midpoint of L-S.
	Fvector H1 = L, H2 = S;
	m_occ_cur.rays++;
	geom_DB.ray_options(CDB::OPT_ONLYNEAREST);
	geom_DB.ray_query(geom_MODEL, L, dir, direct_dist);
	if (geom_DB.r_count())
		H1.mad(L, dir, geom_DB.r_begin()->range);

	Fvector rdir;
	rdir.invert(dir);
	m_occ_cur.rays++;
	geom_DB.ray_options(CDB::OPT_ONLYNEAREST);
	geom_DB.ray_query(geom_MODEL, S, rdir, direct_dist);
	if (geom_DB.r_count())
		H2.mad(S, rdir, geom_DB.r_begin()->range);

	Fvector mid;
	mid.add(H1, H2);
	mid.mul(0.5f);

	// "Wide" candidates: over/around a large obstacle seen from a distance (a wall or silo tens of
	// metres away) -- anchored at the direct line's midpoint, offsets scaled for that case.
	// "Near" candidates (30/09): a doorway/gap only a metre or two from the listener or the source --
	// the wide set's 2-15m/8m offsets routinely miss an opening this close (e.g. standing just inside
	// an open train wagon door in an otherwise wide-open hangar), which was the dominant failure mode
	// in the first in-game test (~6% diffraction success). Anchored at the real blocker hit points
	// (H1/H2) instead of the midpoint, at two much smaller scales, sideways and up (skipping "down" --
	// there's rarely anything to duck under indoors).
	Fvector candidates[17];
	u32 cand_count = 0;
	candidates[cand_count++].mad(mid, up, 2.f);
	candidates[cand_count++].mad(mid, up, 6.f);
	candidates[cand_count++].mad(mid, up, 15.f);
	candidates[cand_count++].mad(mid, right, 8.f);
	candidates[cand_count++].mad(mid, right, -8.f);

	const float near_scales[2] = { 0.6f, 2.f };
	const Fvector* near_anchors[2] = { &H1, &H2 };
	for (u32 a = 0; a < 2; ++a)
	{
		for (u32 sc = 0; sc < 2; ++sc)
		{
			const float o = near_scales[sc];
			candidates[cand_count++].mad(*near_anchors[a], up, o);
			candidates[cand_count++].mad(*near_anchors[a], right, o);
			candidates[cand_count++].mad(*near_anchors[a], right, -o);
		}
	}

	const float lambda_gain = 1.37f; // ~250Hz -- broadband/gain attenuation
	const float lambda_hf = 0.086f;  // ~4kHz -- treble attenuation
	float best_db_gain = flt_max, best_db_hf = flt_max;
	bool found = false;

	for (u32 c = 0; c < cand_count; ++c)
	{
		const Fvector& P = candidates[c];
		Fvector d1;
		d1.sub(P, L);
		const float dist1 = d1.magnitude();
		if (dist1 < 0.05f)
			continue;
		d1.div(dist1);
		m_occ_cur.rays++;
		geom_DB.ray_options(CDB::OPT_ONLYFIRST);
		geom_DB.ray_query(geom_MODEL, L, d1, dist1);
		if (geom_DB.r_count())
			continue; // blocked on the L->P leg

		Fvector d2;
		d2.sub(S, P);
		const float dist2 = d2.magnitude();
		if (dist2 < 0.05f)
			continue;
		d2.div(dist2);
		m_occ_cur.rays++;
		geom_DB.ray_options(CDB::OPT_ONLYFIRST);
		geom_DB.ray_query(geom_MODEL, P, d2, dist2);
		if (geom_DB.r_count())
			continue; // blocked on the P->S leg

		const float path_len = dist1 + dist2;
		const float delta = _max(path_len - direct_dist, 0.f);
		const float N_gain = 2.f * delta / lambda_gain;
		const float N_hf = 2.f * delta / lambda_hf;
		const float db_gain = _min(10.f * log10f(3.f + 20.f * N_gain), 25.f);
		const float db_hf = _min(10.f * log10f(3.f + 20.f * N_hf), 25.f);

		if (db_gain < best_db_gain)
		{
			best_db_gain = db_gain;
			best_db_hf = db_hf;
			found = true;
		}
	}

	if (found)
	{
		res.blocked = false;
		res.diffracted = true;
		res.gain = powf(10.f, -best_db_gain / 20.f);
		res.gain_hf = powf(10.f, -best_db_hf / 20.f);
	}
	return res;
}

CSoundRender_Core::SSoundOcclusionResult CSoundRender_Core::get_occlusion_ex(const Fvector& src, u32 profile, LPCSTR debug_name, bool is_weapon_shot)
{
	occ_scope_timer _t(m_occ_cur.ticks);
	m_occ_cur.calls++;

	SSoundOcclusionResult res;
	if (!geom_MODEL)
		return res;

	const Fvector L = listener_position();
	Fvector dir;
	dir.sub(src, L);
	const float dist = dir.magnitude();
	if (dist < 0.5f)
		return res;
	dir.div(dist);

	// Deterministic sample pattern around the source (no random jitter -> the same shot, from the same
	// spot, sounds the same every time; averaging blocked/clear samples gives soft partial-occlusion
	// edges instead of the old single-ray flicker). profile: 0=light(1 sample) 1=impulse(5) 2=loop(3).
	Fvector up, right;
	Fvector::generate_orthonormal_basis(dir, up, right);
	const float o = 0.35f;
	Fvector offsets[5];
	offsets[0].set(0.f, 0.f, 0.f);
	offsets[1].mul(up, o);
	offsets[2].mul(up, -o);
	offsets[3].mul(right, o);
	offsets[4].mul(right, -o);
	const u32 n = (profile == 1) ? 5 : (profile == 2 ? 3 : 1);

	const float indoor_f = occ_indoor_f();
	const float strength = _max(psSoundOcclusionStrength + (psSoundOcclusionIndoorStrength - psSoundOcclusionStrength) * indoor_f, 0.f);
	float energy = 0.f, hf_db_sum = 0.f, raw_db_sum = 0.f;
	for (u32 s = 0; s < n; ++s)
	{
		Fvector target;
		target.add(src, offsets[s]);
		float loss_db = 0.f, hf_db = 0.f, raw_db = 0.f;
		occ_trace_losses(L, target, loss_db, hf_db, m_occ_cur.rays, &raw_db);
		loss_db *= strength;
		hf_db *= strength;
		energy += powf(10.f, -loss_db / 10.f);
		hf_db_sum += hf_db;
		raw_db_sum += raw_db;
	}
	energy /= float(n);
	res.gain = _sqrt(_max(energy, 0.f)); // power-domain average -> amplitude-domain gain
	res.gain_hf = powf(10.f, -(hf_db_sum / float(n)) / 20.f);
	res.blocked = res.gain < 0.99f;
	if (res.blocked)
		m_occ_cur.blocked++;

	// Diffraction only kicks in once the direct path is meaningfully blocked (~6dB), and never for the
	// "light" profile (footsteps, impacts -- not worth the extra rays). gain=max(direct,diffracted):
	// the better of the two paths wins, it's never an average of "blocked" and "clear".
	if (res.gain < 0.5f && psSoundOcclusionDiffraction && profile != 0)
	{
		SSoundOcclusionResult diff = occ_try_diffraction(L, src, dist);
		if (diff.gain > res.gain)
		{
			res.gain = diff.gain;
			res.gain_hf = _max(res.gain_hf, diff.gain_hf);
			res.diffracted = true;
			res.blocked = false;
		}
	}

	// 30/09: the reverb SEND gets its own, much gentler broadband gain, deliberately NOT equal to the
	// direct path's. A real obstacle blocks the direct line far more than it blocks a room's own
	// reflected/reverberant energy (which reaches the listener via paths this ray-based system never
	// traces at all) -- coupling both to the same gain was what made occlusion feel like a mute switch
	// indoors instead of "duller, still there". wet_sensitivity 0 = reverb send untouched by occlusion,
	// 1 = old fully-coupled behaviour.
	const float wet_sensitivity = _max(0.f, _min(psSoundOcclusionWetSensitivity, 1.f));
	res.wet_gain = 1.f - wet_sensitivity * (1.f - res.gain);

	if (psSoundOcclusionDebug >= 1 && is_weapon_shot)
	{
		// raw= mean direct-path material sum BEFORE the cap and before strength (how far past the cap the
		// geometry really goes); ind= the indoor factor in effect (0 outdoor .. 1 indoor).
		Msg("* [snd_occ] %s dist=%.1f gain=%.2f (%.1fdB) wet=%.2f hf=%.2f (%.1fdB) %s%s raw=%.1fdB ind=%.2f",
			debug_name ? debug_name : "?", dist, res.gain, -20.f * log10f(_max(res.gain, 0.0001f)),
			res.wet_gain, res.gain_hf, -20.f * log10f(_max(res.gain_hf, 0.0001f)),
			res.blocked ? "BLOCKED" : "clear", res.diffracted ? " (diffracted)" : "",
			raw_db_sum / float(n), indoor_f);
	}

	return res;
}
