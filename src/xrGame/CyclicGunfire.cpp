#include "stdafx.h"
#include "CyclicGunfire.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// Port of the config reading / fallback cascade of zzzzz_sound_loop_custom.script (TRUE CYCLIC V2), so the native
// voice and the scripted fallback pick exactly the same samples for the same weapon state.

namespace CyclicGunfire
{
	namespace
	{
		// ------------------------------------------------------------------------------------------------------
		// small parsing helpers (mirror the script's trim / r_string_or_nil / parse_sound_variants ...)
		// ------------------------------------------------------------------------------------------------------

		xr_string trim(const char* s)
		{
			if (!s) return xr_string();
			const char* b = s;
			while (*b && isspace((unsigned char)*b)) ++b;
			const char* e = b + strlen(b);
			while (e > b && isspace((unsigned char)e[-1])) --e;
			return xr_string(b, (size_t)(e - b));
		}

		xr_string lower(xr_string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
			return s;
		}

		bool is_no_sound(const xr_string& p)
		{
			xr_string l = lower(trim(p.c_str()));
			return l.empty() || l == "%no_sound" || l == "$no_sound" || l == "no_sound" || l == "%no_sound.ogg" ||
				l == "$no_sound.ogg" || l == "no_sound.ogg";
		}

		bool r_str(const char* sec, const xr_string& key, xr_string& out)
		{
			if (!pSettings->line_exist(sec, key.c_str())) return false;
			xr_string v = trim(pSettings->r_string(sec, key.c_str()));
			if (v.empty()) return false;
			out = v;
			return true;
		}

		bool r_num(const char* sec, const xr_string& key, float& out)
		{
			xr_string v;
			if (!r_str(sec, key, v)) return false;
			char* endp = nullptr;
			double d = strtod(v.c_str(), &endp);
			if (endp == v.c_str()) return false;
			out = (float)d;
			return true;
		}

		bool r_flag(const char* sec, const xr_string& key, bool& out)
		{
			if (!pSettings->line_exist(sec, key.c_str())) return false;
			out = pSettings->r_bool(sec, key.c_str()) != FALSE;
			return true;
		}

		int r_curve(const char* sec, const char* key)
		{
			xr_string v;
			if (r_str(sec, key, v))
			{
				v = lower(v);
				if (v == "linear") return 0;
				if (v == "equal_power" || v == "equalpower") return 1;
			}
			return 1;
		}

		void split(const xr_string& s, xr_vector<xr_string>& out)
		{
			size_t pos = 0;
			while (pos <= s.size())
			{
				size_t c = s.find(',', pos);
				if (c == xr_string::npos) c = s.size();
				out.push_back(trim(s.substr(pos, c - pos).c_str()));
				pos = c + 1;
			}
		}

		// "a, b, c" -> variants without the no_sound entries (empty when nothing is left)
		void parse_variants(const xr_string& raw, xr_vector<xr_string>& out)
		{
			out.clear();
			xr_vector<xr_string> tok;
			split(raw, tok);
			for (auto& t : tok)
				if (!t.empty() && !is_no_sound(t)) out.push_back(t);
		}

		// ------------------------------------------------------------------------------------------------------
		// configuration
		// ------------------------------------------------------------------------------------------------------

		struct SSet
		{
			xr_vector<xr_string> start;
			xr_vector<xr_string> end_;
			xr_string loop_path;
			bool has_shots = false;
			float shots = 0.f;
			bool has_rpm = false;
			float rpm = 0.f;
		};

		struct SResolved
		{
			const xr_vector<xr_string>* start = nullptr;
			const xr_vector<xr_string>* end_ = nullptr;
			xr_string loop;
			float shots = 0.f;
			float rpm = 600.f;
			bool valid = false;
		};

		struct SConfig
		{
			xr_string section;
			xr_map<xr_string, SSet> sets;   // "" = base, then "_suppressed", "_indoor", ...
			xr_map<xr_string, SSet> trans;  // "outdoor_to_indoor", "indoor_to_outdoor_suppressed", ...
			bool subsonic_enabled = false;
			bool transition_enable = false;
			bool transition_ads_enable = false;
			float transition_debounce_ms = 750.f;
			float ads_cooldown_cycles = 1.f;
			float loop_delay_s = 0.f, loop_delay_s_supp = -1.f;
			float end_margin_s = 0.f, end_margin_s_supp = -1.f;
			bool start_cutoff_on_loop = true;
			bool start_cutoff_soft = false;
			bool start_cutoff_fade_enable = true;
			float start_cutoff_fade_s = 0.03f;
			int start_cutoff_fade_curve = 1;
			bool loop_fadeout_enable = false;
			float loop_fadeout_s = 0.06f;
			int loop_fadeout_curve = 1;
			bool end_fadein_enable = false;
			float end_fadein_s = 0.03f;
			int end_fadein_curve = 1;
			SResolved cache[16];
			bool cached[16] = {};
		};

		const char* VARIANT_SUFFIXES[] = {"_suppressed", "_indoor", "_suppressed_indoor", "_ads", "_suppressed_ads",
			"_ads_indoor", "_suppressed_ads_indoor"};
		const char* SUBSONIC_SUFFIXES[] = {"_subsonic", "_subsonic_indoor", "_subsonic_ads", "_subsonic_ads_indoor"};
		const char* TRANSITION_NAMES[] = {"outdoor_to_indoor", "outdoor_to_indoor_suppressed", "outdoor_to_indoor_subsonic",
			"indoor_to_outdoor", "indoor_to_outdoor_suppressed", "indoor_to_outdoor_subsonic"};

		// "path, shots, rpm" -> SSet loop fields (variant sets / transitions). path stays empty for no_sound.
		void parse_triplet(const xr_string& raw, SSet& s)
		{
			xr_vector<xr_string> p;
			split(raw, p);
			if (p.size() >= 1 && !is_no_sound(p[0])) s.loop_path = p[0];
			if (p.size() >= 2 && !p[1].empty())
			{
				char* e = nullptr;
				double d = strtod(p[1].c_str(), &e);
				if (e != p[1].c_str()) { s.has_shots = true; s.shots = (float)d; }
			}
			if (p.size() >= 3 && !p[2].empty())
			{
				char* e = nullptr;
				double d = strtod(p[2].c_str(), &e);
				if (e != p[2].c_str()) { s.has_rpm = true; s.rpm = (float)d; }
			}
		}

		void read_variant_set(const char* sec, const char* sfx, SConfig& cfg)
		{
			SSet s;
			xr_string raw;
			if (r_str(sec, xr_string("snd_shoot_start") + sfx, raw)) parse_variants(raw, s.start);
			if (r_str(sec, xr_string("snd_shoot_end") + sfx, raw)) parse_variants(raw, s.end_);
			if (r_str(sec, xr_string("snd_shoot_loop") + sfx, raw)) parse_triplet(raw, s);
			cfg.sets[sfx] = s;
		}

		xr_map<xr_string, SConfig*> g_cfg;

		SConfig* load_config(const char* sec)
		{
			if (!pSettings->section_exist(sec)) return nullptr;
			bool flag = false;
			if (!r_flag(sec, "custom_loop_sound", flag) || !flag) return nullptr;

			// base set -- same validity rules as load_section_config() in the script
			xr_string loop_str;
			if (!r_str(sec, "snd_shoot_loop", loop_str)) loop_str = "$no_sound, 0, 600";
			xr_vector<xr_string> lp;
			split(loop_str, lp);
			SSet base;
			if (!lp.empty() && !is_no_sound(lp[0])) base.loop_path = lp[0];
			base.has_shots = base.has_rpm = true;
			base.shots = 1.f;
			base.rpm = 600.f;
			if (lp.size() >= 2) { char* e = nullptr; double d = strtod(lp[1].c_str(), &e); if (e != lp[1].c_str()) base.shots = (float)d; }
			if (lp.size() >= 3) { char* e = nullptr; double d = strtod(lp[2].c_str(), &e); if (e != lp[2].c_str()) base.rpm = (float)d; }

			xr_string start_path, end_path;
			bool has_start = r_str(sec, "snd_shoot_start", start_path);
			bool has_end = r_str(sec, "snd_shoot_end", end_path);
			if (!has_start || is_no_sound(start_path)) return nullptr;
			if (!has_end || is_no_sound(end_path))
			{
				if (!base.loop_path.empty()) return nullptr; // a full-auto weapon needs an end sound
				end_path = "$no_sound";
			}
			parse_variants(start_path, base.start);
			parse_variants(end_path, base.end_);
			if (base.start.empty()) return nullptr;

			SConfig* cfg = new SConfig();
			cfg->section = sec;
			cfg->sets[""] = base;
			for (const char* sfx : VARIANT_SUFFIXES) read_variant_set(sec, sfx, *cfg);
			bool sub = false;
			r_flag(sec, "subsonic_enabled", sub);
			cfg->subsonic_enabled = sub;
			if (sub)
				for (const char* sfx : SUBSONIC_SUFFIXES) read_variant_set(sec, sfx, *cfg);
			for (const char* name : TRANSITION_NAMES)
			{
				SSet t;
				xr_string raw;
				if (r_str(sec, xr_string("snd_shoot_loop_") + name, raw)) parse_triplet(raw, t);
				cfg->trans[name] = t;
			}

			float f;
			bool b;
			if (r_num(sec, "snd_shoot_mode_transition_debounce", f)) cfg->transition_debounce_ms = f * 1000.f;
			if (r_num(sec, "snd_shoot_mode_transition_ads_cooldown_cycles", f)) cfg->ads_cooldown_cycles = f;
			if (r_flag(sec, "snd_shoot_mode_transition_enable", b)) cfg->transition_enable = b;
			if (r_flag(sec, "snd_shoot_mode_transition_ads_enable", b)) cfg->transition_ads_enable = b;

			if (r_num(sec, "snd_shoot_loop_delay", f)) cfg->loop_delay_s = f;
			if (r_num(sec, "snd_shoot_loop_delay_suppressed", f)) cfg->loop_delay_s_supp = f;
			if (r_num(sec, "snd_shoot_end_cycle_margin", f)) cfg->end_margin_s = f;
			if (r_num(sec, "snd_shoot_end_cycle_margin_suppressed", f)) cfg->end_margin_s_supp = f;

			// snd_shoot_start_cutoff_on_loop defaults to true: only an explicit false turns it off
			if (r_flag(sec, "snd_shoot_start_cutoff_on_loop", b)) cfg->start_cutoff_on_loop = b;
			cfg->start_cutoff_soft = r_flag(sec, "snd_shoot_start_cutoff_soft", b) && b;
			if (r_flag(sec, "snd_shoot_start_cutoff_fade_enable", b)) cfg->start_cutoff_fade_enable = b;
			if (r_num(sec, "snd_shoot_start_cutoff_fade", f)) cfg->start_cutoff_fade_s = f;
			cfg->start_cutoff_fade_curve = r_curve(sec, "snd_shoot_start_cutoff_fade_curve");

			cfg->loop_fadeout_enable = r_flag(sec, "snd_shoot_loop_fadeout_enable", b) && b;
			if (r_num(sec, "snd_shoot_loop_fadeout", f)) cfg->loop_fadeout_s = f;
			cfg->loop_fadeout_curve = r_curve(sec, "snd_shoot_loop_fadeout_curve");

			cfg->end_fadein_enable = r_flag(sec, "snd_shoot_end_fadein_enable", b) && b;
			if (r_num(sec, "snd_shoot_end_fadein", f)) cfg->end_fadein_s = f;
			cfg->end_fadein_curve = r_curve(sec, "snd_shoot_end_fadein_curve");
			return cfg;
		}

		SConfig* get_config(const char* sec)
		{
			auto it = g_cfg.find(sec);
			if (it != g_cfg.end()) return it->second;
			SConfig* c = load_config(sec);
			g_cfg[sec] = c;
			return c;
		}

		// Fallback cascade -- identical to build_chain() of the script ("" = the base key).
		void build_chain(bool supp, bool subsonic, bool ads, bool indoor, xr_vector<const char*>& c)
		{
			c.clear();
			auto set = [&c](std::initializer_list<const char*> l) { for (const char* s : l) c.push_back(s); };
			if (supp && subsonic && ads && indoor)
				set({"_subsonic_ads_indoor", "_subsonic_indoor", "_subsonic_ads", "_suppressed_ads_indoor", "_subsonic",
				     "_suppressed_indoor", "_suppressed_ads", "_ads_indoor", "_suppressed", "_indoor", "_ads", ""});
			else if (supp && subsonic && ads)
				set({"_subsonic_ads", "_subsonic", "_suppressed_ads", "_suppressed", "_ads", ""});
			else if (supp && subsonic && indoor)
				set({"_subsonic_indoor", "_subsonic", "_suppressed_indoor", "_suppressed", "_indoor", ""});
			else if (supp && ads && indoor)
				set({"_suppressed_ads_indoor", "_suppressed_ads", "_ads_indoor", "_suppressed_indoor", "_ads", "_suppressed",
				     "_indoor", ""});
			else if (supp && subsonic)
				set({"_subsonic", "_suppressed", ""});
			else if (supp && ads)
				set({"_suppressed_ads", "_ads", "_suppressed", ""});
			else if (ads && indoor)
				set({"_ads_indoor", "_ads", "_indoor", ""});
			else if (supp && indoor)
				set({"_suppressed_indoor", "_suppressed", "_indoor", ""});
			else if (ads)
				set({"_ads", ""});
			else if (supp)
				set({"_suppressed", ""});
			else if (indoor)
				set({"_indoor", ""});
			else if (subsonic)
				set({"_subsonic", ""});
			else
				set({""});
		}

		// Each field is resolved on its own, first non-nil along the chain (like resolve() in the script).
		const SResolved& resolve(SConfig& cfg, bool supp, bool subsonic, bool ads, bool indoor)
		{
			const int mask = (supp ? 1 : 0) | (subsonic ? 2 : 0) | (ads ? 4 : 0) | (indoor ? 8 : 0);
			if (cfg.cached[mask]) return cfg.cache[mask];
			xr_vector<const char*> chain;
			build_chain(supp, subsonic, ads, indoor, chain);
			SResolved r;
			bool got_loop = false, got_shots = false, got_rpm = false;
			for (const char* sfx : chain)
			{
				auto it = cfg.sets.find(sfx);
				if (it == cfg.sets.end()) continue;
				const SSet& s = it->second;
				if (!r.start && !s.start.empty()) r.start = &s.start;
				if (!r.end_ && !s.end_.empty()) r.end_ = &s.end_;
				if (!got_loop && !s.loop_path.empty()) { r.loop = s.loop_path; got_loop = true; }
				if (!got_shots && s.has_shots) { r.shots = s.shots; got_shots = true; }
				if (!got_rpm && s.has_rpm) { r.rpm = s.rpm; got_rpm = true; }
			}
			r.valid = true;
			cfg.cache[mask] = r;
			cfg.cached[mask] = true;
			return cfg.cache[mask];
		}

		shared_str pick(const xr_vector<xr_string>* v)
		{
			if (!v || v->empty()) return shared_str();
			if (v->size() == 1) return shared_str((*v)[0].c_str());
			return shared_str((*v)[::Random.randI((u32)v->size())].c_str());
		}

		// ------------------------------------------------------------------------------------------------------
		// burst state
		// ------------------------------------------------------------------------------------------------------

		struct SSession
		{
			bool active = false;
			u16 wid = 0;
			xr_string section;
			bool supp = false;
			bool subsonic = false;
			bool loop_indoor = false, loop_ads = false; // mode of the loop sample in use
			bool track_indoor = false, track_ads = false;
			u32 unlock_ms = 0;
			u32 ads_lock_ms = 0;
		} g_s;

		bool g_indoor = false;

		void fill_common(const SConfig& cfg, const SShotCtx& c, bool supp, SCyclicEvent& e)
		{
			e.owner = c.owner;
			e.ai_type = c.ai_type;
			e.shot_no = c.shot_no;
			e.last_shot = c.last_shot;
			e.real_rpm = c.real_rpm;
			e.loop_delay_s = (supp && cfg.loop_delay_s_supp >= 0.f) ? cfg.loop_delay_s_supp : cfg.loop_delay_s;
			e.end_margin_s = (supp && cfg.end_margin_s_supp >= 0.f) ? cfg.end_margin_s_supp : cfg.end_margin_s;
			e.start_cutoff_on_loop = cfg.start_cutoff_on_loop;
			e.start_cutoff_fade_enable = cfg.start_cutoff_fade_enable;
			e.start_cutoff_soft = cfg.start_cutoff_soft;
			e.start_cutoff_fade_s = cfg.start_cutoff_fade_s;
			e.start_cutoff_fade_curve = cfg.start_cutoff_fade_curve;
			e.loop_fadeout_enable = cfg.loop_fadeout_enable;
			e.loop_fadeout_s = cfg.loop_fadeout_s;
			e.loop_fadeout_curve = cfg.loop_fadeout_curve;
			e.end_fadein_enable = cfg.end_fadein_enable;
			e.end_fadein_s = cfg.end_fadein_s;
			e.end_fadein_curve = cfg.end_fadein_curve;
		}
	}

	bool is_cyclic_section(const char* section)
	{
		return get_config(section) != nullptr;
	}

	bool wants_subsonic_scan(const char* section)
	{
		SConfig* cfg = get_config(section);
		return cfg && cfg->subsonic_enabled;
	}

	bool make_shot_event(const SShotCtx& c, SCyclicEvent& out)
	{
		SConfig* cfg = get_config(c.section);
		if (!cfg) return false;

		const u32 now = Device.dwTimeGlobal;
		const bool ind = g_indoor;
		const bool begin = (c.shot_no <= 1) || !g_s.active || g_s.wid != c.weapon_id;

		if (begin)
		{
			g_s = SSession();
			g_s.active = true;
			g_s.wid = c.weapon_id;
			g_s.section = c.section;
			g_s.supp = c.suppressed;
			g_s.subsonic = c.suppressed && cfg->subsonic_enabled && c.all_subsonic;
			g_s.loop_indoor = g_s.track_indoor = ind;
			g_s.loop_ads = g_s.track_ads = c.ads;
		}

		out = SCyclicEvent();
		out.type = begin ? cyc_begin : cyc_shot;
		fill_common(*cfg, c, g_s.supp, out);

		// indoor <-> outdoor and ADS swaps while the loop runs (script: schedule_mode_swap / execute_mode_swap)
		if (!begin && c.shot_no >= 3)
		{
			bool swap = false, indoor_changed = false;
			if (ind != g_s.track_indoor)
			{
				if (cfg->transition_enable)
				{
					if (now >= g_s.unlock_ms) { swap = true; indoor_changed = true; }
				}
				else
					g_s.track_indoor = ind;
			}
			if (c.ads != g_s.track_ads)
			{
				if (now < g_s.ads_lock_ms)
				{
					// adaptation cooldown after an indoor transition: ignore
				}
				else if (cfg->transition_ads_enable)
				{
					if (now >= g_s.unlock_ms) swap = true;
				}
				else
					g_s.track_ads = c.ads;
			}
			if (swap)
			{
				bool used_trans = false;
				if (indoor_changed)
				{
					xr_string name = ind ? "outdoor_to_indoor" : "indoor_to_outdoor";
					if (g_s.supp && g_s.subsonic) name += "_subsonic";
					else if (g_s.supp) name += "_suppressed";
					auto it = cfg->trans.find(name);
					if (it != cfg->trans.end() && !it->second.loop_path.empty())
					{
						out.trans_name = it->second.loop_path.c_str();
						out.trans_shots = it->second.has_shots ? (int)it->second.shots : 3;
						out.trans_sample_rpm = (it->second.has_rpm && it->second.rpm > 0.f) ? it->second.rpm : 600.f;
						used_trans = true;
					}
				}
				g_s.loop_indoor = g_s.track_indoor = ind;
				g_s.loop_ads = g_s.track_ads = c.ads;
				g_s.unlock_ms = now + (u32)cfg->transition_debounce_ms;
				if (used_trans)
					g_s.ads_lock_ms = now + (u32)(60000.f / std::max(1.f, c.real_rpm) * cfg->ads_cooldown_cycles);
			}
		}

		const SResolved& RL = resolve(*cfg, g_s.supp, g_s.subsonic, g_s.loop_ads, g_s.loop_indoor);
		if (begin)
			out.start_name = pick(RL.start);
		out.loop_name = RL.loop.empty() ? shared_str() : shared_str(RL.loop.c_str());
		out.loop_shots = (int)RL.shots;
		out.loop_sample_rpm = RL.rpm > 0.f ? RL.rpm : 600.f;
		if (out.loop_name.size() == 0 && !begin)
			out.start_name = pick(RL.start);

		// the tail depends on the state at the END of the burst (current indoor / ADS), supp & subsonic of the burst
		const SResolved& RE = resolve(*cfg, g_s.supp, g_s.subsonic, c.ads, ind);
		out.end_name = pick(RE.end_);

		if (c.last_shot)
			g_s.active = false; // the voice closes the burst by itself; a later FireEnd must not close it twice
		return true;
	}

	bool make_release_event(u16 weapon_id, bool ads, CObject* owner, int ai_type, float real_rpm, SCyclicEvent& out)
	{
		if (!g_s.active || g_s.wid != weapon_id) return false;
		SConfig* cfg = get_config(g_s.section.c_str());
		if (!cfg)
		{
			g_s.active = false;
			return false;
		}
		SShotCtx c;
		c.section = g_s.section.c_str();
		c.weapon_id = weapon_id;
		c.owner = owner;
		c.ai_type = ai_type;
		c.real_rpm = real_rpm;
		out = SCyclicEvent();
		out.type = cyc_release;
		fill_common(*cfg, c, g_s.supp, out);
		const SResolved& RE = resolve(*cfg, g_s.supp, g_s.subsonic, ads, g_indoor);
		out.end_name = pick(RE.end_);
		g_s.active = false;
		return true;
	}

	bool session_active(u16 weapon_id)
	{
		return g_s.active && g_s.wid == weapon_id;
	}

	void end_session(u16 weapon_id)
	{
		if (g_s.wid == weapon_id) g_s.active = false;
	}

	void set_indoor(bool v) { g_indoor = v; }
	bool get_indoor() { return g_indoor; }
}
