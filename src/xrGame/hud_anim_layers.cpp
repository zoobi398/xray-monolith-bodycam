#include "stdafx.h"
#include "hud_anim_layers.h"
#include "player_hud.h"
#include "HudItem.h"
#include "Weapon.h"

#include <algorithm>
#include <cctype>

BOOL g_hudlayers_debug = FALSE; // console: g_hudlayers_debug

namespace
{
	// '*' matches any run of characters (also empty); case-insensitive
	bool wild_match(const char* p, const char* s)
	{
		while (*p)
		{
			if (*p == '*')
			{
				while (*p == '*') ++p;
				if (!*p) return true;
				for (;; ++s)
				{
					if (wild_match(p, s)) return true;
					if (!*s) break;
				}
				return false;
			}
			if (!*s || tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
			++p;
			++s;
		}
		return *s == 0;
	}

	xr_string trim_token(const char* t)
	{
		while (*t && isspace((unsigned char)*t)) ++t;
		const char* e = t + strlen(t);
		while (e > t && isspace((unsigned char)e[-1])) --e;
		return xr_string(t, (size_t)(e - t));
	}
}

CHudAnimLayers::CHudAnimLayers() : m_owner(nullptr), m_ka(nullptr), m_count(0), m_burst_active(false)
{
}

bool CHudAnimLayers::blend_alive(const CBlend* b, const MotionID& mid, u8 channel)
{
	return b && b->blend_state() != CBlend::eFREE_SLOT && b->motionID == mid && b->channel == channel;
}

float CHudAnimLayers::weapon_rpm() const
{
	if (m_owner && m_owner->m_parent_hud_item && m_owner->m_parent_hud_item->has_object())
	{
		CWeapon* w = smart_cast<CWeapon*>(&m_owner->m_parent_hud_item->object());
		if (w)
		{
			const float r = w->RealRPMScript();
			if (r > 1.f && r < 20000.f) return r;
		}
	}
	return 600.f;
}

bool CHudAnimLayers::read_layer(const shared_str& hud_section, const shared_str& layer_section, SLayer& L)
{
	LPCSTR sec = layer_section.c_str();
	if (!pSettings->section_exist(sec))
	{
		Msg("! [hud_layers] %s: layer section [%s] does not exist, skipping", hud_section.c_str(), sec);
		return false;
	}
	L.section = layer_section;

	// motions (variants, picked at random at each trigger)
	if (!pSettings->line_exist(sec, "motion"))
	{
		Msg("! [hud_layers] %s: layer [%s] has no 'motion', skipping", hud_section.c_str(), sec);
		return false;
	}
	{
		LPCSTR list = pSettings->r_string(sec, "motion");
		const int n = _GetItemCount(list);
		for (int i = 0; i < n && L.motion_count < 8; ++i)
		{
			string256 tok;
			_GetItem(list, i, tok);
			xr_string name = trim_token(tok);
			if (name.empty()) continue;
			MotionID mid = m_ka->ID_Cycle_Safe(name.c_str());
			if (!mid.valid())
			{
				Msg("! [hud_layers] %s: layer [%s]: motion '%s' not found in the weapon model", hud_section.c_str(), sec,
				    name.c_str());
				continue;
			}
			L.motions[L.motion_count++] = mid;
		}
		if (!L.motion_count) return false;
	}

	int ch = READ_IF_EXISTS(pSettings, r_s32, sec, "channel", 3);
	if (ch != 2 && ch != 3)
	{
		Msg("! [hud_layers] %s: layer [%s]: channel must be 2 or 3 (got %d), using 3", hud_section.c_str(), sec, ch);
		ch = 3;
	}
	L.channel = (u8)ch;

	xr_string rt = trim_token(READ_IF_EXISTS(pSettings, r_string, sec, "retrigger", "crossfade"));
	std::transform(rt.begin(), rt.end(), rt.begin(), [](unsigned char c) { return (char)tolower(c); });
	if (rt == "restart") L.retrigger = hlr_restart;
	else if (rt == "ignore") L.retrigger = hlr_ignore;
	else L.retrigger = hlr_crossfade;

	L.speed = READ_IF_EXISTS(pSettings, r_float, sec, "speed", 1.f);
	L.speed_rpm = READ_IF_EXISTS(pSettings, r_bool, sec, "speed_rpm", false) != FALSE;
	L.blend_in = _max(READ_IF_EXISTS(pSettings, r_float, sec, "blend_in", 0.03f), 0.001f);
	L.blend_out = _max(READ_IF_EXISTS(pSettings, r_float, sec, "blend_out", 0.10f), 0.001f);
	L.start_min = std::min(std::max(READ_IF_EXISTS(pSettings, r_float, sec, "start_min", 0.f), 0.f), 0.99f);
	L.start_max = std::min(std::max(READ_IF_EXISTS(pSettings, r_float, sec, "start_max", L.start_min), L.start_min), 0.99f);
	L.part = READ_IF_EXISTS(pSettings, r_s32, sec, "part", -1);
	L.max_instances = (u8)std::min<int>(std::max<int>(READ_IF_EXISTS(pSettings, r_s32, sec, "max_instances", 3), 1), MAX_INSTANCES);

	const float power = READ_IF_EXISTS(pSettings, r_float, sec, "power", 1.f);
	m_ka->LL_SetChannelFactor(L.channel, power); // shared by the whole channel: one kind of layer per channel

	// triggers: shot, burst_start, fire_end, anim:<name or pattern with *>, mark:<name>
	if (pSettings->line_exist(sec, "trigger"))
	{
		LPCSTR list = pSettings->r_string(sec, "trigger");
		const int n = _GetItemCount(list);
		for (int i = 0; i < n; ++i)
		{
			string256 tok;
			_GetItem(list, i, tok);
			xr_string t = trim_token(tok);
			if (t.empty()) continue;
			if (t == "shot") L.on_shot = true;
			else if (t == "burst_start") L.on_burst_start = true;
			else if (t == "fire_end") L.on_fire_end = true;
			else if (t.compare(0, 5, "anim:") == 0 && t.size() > 5) L.anim_patterns.push_back(shared_str(t.c_str() + 5));
			else if (t.compare(0, 5, "mark:") == 0 && t.size() > 5) L.mark_names.push_back(shared_str(t.c_str() + 5));
			else
				Msg("! [hud_layers] %s: layer [%s]: unknown trigger '%s' (expected shot, burst_start, fire_end, anim:<name>, mark:<name>)",
				    hud_section.c_str(), sec, t.c_str());
		}
	}
	return true;
}

void CHudAnimLayers::install(attachable_hud_item* owner, const shared_str& hud_section)
{
	remove();

	if (!owner || !owner->m_model) return;
	if (!pSettings->line_exist(hud_section, "hud_layers")) return;

	IKinematicsAnimated* ka = owner->m_model->dcast_PKinematicsAnimated();
	if (!ka) return;

	m_owner = owner;
	m_ka = ka;

	LPCSTR list = pSettings->r_string(hud_section, "hud_layers");
	const int n = _GetItemCount(list);
	for (int i = 0; i < n && m_count < MAX_LAYERS; ++i)
	{
		string256 tok;
		_GetItem(list, i, tok);
		xr_string name = trim_token(tok);
		if (name.empty()) continue;
		SLayer L;
		if (!read_layer(hud_section, shared_str(name.c_str()), L)) continue;
		m_layers[m_count++] = L;
	}

	if (!m_count)
	{
		m_owner = nullptr;
		m_ka = nullptr;
		return;
	}
	if (g_hudlayers_debug)
		Msg("* [hud_layers] %s: %d layer(s) installed", hud_section.c_str(), (int)m_count);
}

void CHudAnimLayers::remove()
{
	// The blends belong to the weapon model and die with it: only forget our pointers.
	for (u8 i = 0; i < MAX_LAYERS; ++i)
		m_layers[i] = SLayer();
	m_count = 0;
	m_owner = nullptr;
	m_ka = nullptr;
	m_burst_active = false;
}

void CHudAnimLayers::fire(SLayer& L)
{
	if (!m_ka || !L.motion_count) return;

	// forget the instances that no longer exist (the engine recycles blend slots)
	for (int i = (int)L.inst_count - 1; i >= 0; --i)
	{
		SInstance& I = L.inst[i];
		bool alive = false;
		for (u8 k = 0; k < I.count; ++k)
			if (blend_alive(I.blend[k], I.mid, L.channel)) { alive = true; break; }
		if (!alive)
		{
			for (int j = i; j < (int)L.inst_count - 1; ++j) L.inst[j] = L.inst[j + 1];
			--L.inst_count;
		}
	}

	// is an instance still playing (not already fading out at its end)?
	bool playing = false;
	for (u8 i = 0; i < L.inst_count && !playing; ++i)
		for (u8 k = 0; k < L.inst[i].count; ++k)
		{
			const CBlend* b = L.inst[i].blend[k];
			if (blend_alive(b, L.inst[i].mid, L.channel) && b->blend_state() == CBlend::eAccrue) { playing = true; break; }
		}

	if (L.retrigger == hlr_ignore && playing) return;

	if (L.retrigger == hlr_restart && playing && L.inst_count)
	{
		SInstance& I = L.inst[L.inst_count - 1];
		for (u8 k = 0; k < I.count; ++k)
		{
			CBlend* b = I.blend[k];
			if (!blend_alive(b, I.mid, L.channel)) continue;
			b->timeCurrent = ::Random.randF(L.start_min, L.start_max) * b->timeTotal;
			b->playing = TRUE;
			b->set_accrue_state();
		}
		return;
	}

	if (L.retrigger == hlr_crossfade)
	{
		// the running instances fade out while the new one comes in (inside a channel they are interpolated by weight)
		for (u8 i = 0; i < L.inst_count; ++i)
			for (u8 k = 0; k < L.inst[i].count; ++k)
			{
				CBlend* b = L.inst[i].blend[k];
				if (blend_alive(b, L.inst[i].mid, L.channel) && b->blend_state() == CBlend::eAccrue)
				{
					b->set_falloff_state();
					b->blendFalloff = 1.f / L.blend_out;
				}
			}
	}

	// never keep more than max_instances alive: the oldest ones leave fast
	while (L.inst_count >= L.max_instances)
	{
		SInstance& O = L.inst[0];
		for (u8 k = 0; k < O.count; ++k)
		{
			CBlend* b = O.blend[k];
			if (blend_alive(b, O.mid, L.channel))
			{
				b->set_falloff_state();
				b->blendFalloff = 40.f;
			}
		}
		for (int j = 0; j < (int)L.inst_count - 1; ++j) L.inst[j] = L.inst[j + 1];
		--L.inst_count;
	}

	// pick a variant (not the same twice in a row when there are several)
	u8 v = 0;
	if (L.motion_count > 1)
	{
		v = (u8)::Random.randI(L.motion_count);
		if (v == L.last_variant) v = (u8)((v + 1 + ::Random.randI(L.motion_count - 1)) % L.motion_count);
	}
	L.last_variant = v;
	const MotionID mid = L.motions[v];

	CMotionDef* md = m_ka->LL_GetMotionDef(mid);
	const float def_speed = (md && md->Speed() > 0.f) ? md->Speed() : 1.f;
	const float rpm = L.speed_rpm ? weapon_rpm() : 0.f;

	SInstance I;
	I.count = 0;
	I.mid = mid;
	for (u8 k = 0; k < MAX_PARTS_USED; ++k) I.blend[k] = nullptr;

	const u16 pc = std::min<u16>(m_ka->partitions().count(), MAX_PARTS_USED);
	for (u16 pid = 0; pid < pc; ++pid)
	{
		if (L.part >= 0 && pid != (u16)L.part) continue;
		CBlend* B = m_ka->LL_PlayCycle(pid, mid, TRUE /*mixin*/, 1.f / L.blend_in, 0.f, L.speed * def_speed, TRUE /*no loop*/,
		                               nullptr, nullptr, L.channel);
		if (!B) continue; // bone full or blend pool exhausted: the engine ignores it silently, so do we
		if (L.speed_rpm)
			B->speed = B->timeTotal * (rpm / 60.f) * L.speed; // one pass lasts exactly one shot interval
		if (L.start_max > 0.f)
			B->timeCurrent = ::Random.randF(L.start_min, L.start_max) * B->timeTotal;
		I.blend[I.count++] = B;
	}
	if (I.count)
		L.inst[L.inst_count++] = I;

	if (g_hudlayers_debug)
		Msg("* [hud_layers] %s: fired (variant %d/%d, %d partition blend(s), %d instance(s) alive)", L.section.c_str(), (int)v + 1,
		    (int)L.motion_count, (int)I.count, (int)L.inst_count);
}

void CHudAnimLayers::on_shot(int shot_no)
{
	if (!m_count) return;
	m_burst_active = true;
	for (u8 i = 0; i < m_count; ++i)
	{
		SLayer& L = m_layers[i];
		if (L.on_shot || (L.on_burst_start && shot_no <= 1))
			fire(L);
	}
}

void CHudAnimLayers::on_fire_end()
{
	if (!m_count || !m_burst_active) return;
	m_burst_active = false; // fire_end only follows a burst that really fired (FireEnd is also called while idle)
	for (u8 i = 0; i < m_count; ++i)
		if (m_layers[i].on_fire_end) fire(m_layers[i]);
}

void CHudAnimLayers::on_anim(const shared_str& alias)
{
	if (!m_count || !alias.size()) return;
	for (u8 i = 0; i < m_count; ++i)
	{
		SLayer& L = m_layers[i];
		for (const shared_str& pat : L.anim_patterns)
			if (wild_match(pat.c_str(), alias.c_str()))
			{
				fire(L);
				break;
			}
	}
}

void CHudAnimLayers::on_mark(const shared_str& mark)
{
	if (!m_count || !mark.size()) return;
	for (u8 i = 0; i < m_count; ++i)
	{
		SLayer& L = m_layers[i];
		for (const shared_str& m : L.mark_names)
			if (wild_match(m.c_str(), mark.c_str()))
			{
				fire(L);
				break;
			}
	}
}

bool CHudAnimLayers::play_by_name(const char* layer_section)
{
	if (!m_count || !layer_section) return false;
	for (u8 i = 0; i < m_count; ++i)
		if (m_layers[i].section == layer_section)
		{
			fire(m_layers[i]);
			return true;
		}
	return false;
}
