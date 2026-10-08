#pragma once
// Cyclic gunfire in the engine (doc 08_ENGINE_CHANGES_CYCLIC_GUNFIRE_IN_ENGINE.md).
//
// Weapon-side half of the native cyclic voice: reads the same .ltx keys the scripted TRUE CYCLIC system reads
// (custom_loop_sound, snd_shoot_start / _loop / _end and their _suppressed / _indoor / _ads / _subsonic variants,
// transitions, timing, fades), resolves which samples apply to the weapon's current state, and turns every shot
// into an SCyclicEvent for the voice that lives in xrSound. Nothing here touches Lua per shot.

#include "../xrSound/Sound.h"

namespace CyclicGunfire
{
	// What the weapon tells us about itself at a shot.
	struct SShotCtx
	{
		const char* section = nullptr;
		u16 weapon_id = 0;
		CObject* owner = nullptr;
		int ai_type = 0;
		int shot_no = 0;        // 1-based inside the burst (CWeaponMagazined::m_iShotNum)
		bool last_shot = false; // burst limit reached or last round of the magazine
		float real_rpm = 600.f;
		bool suppressed = false;
		bool ads = false;
		bool all_subsonic = false; // every cartridge in the magazine is subsonic (only asked when the weapon opts in)
	};

	// True when the section is a valid cyclic weapon (custom_loop_sound = true and a usable sample set).
	bool is_cyclic_section(const char* section);

	// Does this section need the magazine scan for subsonic ammo (subsonic_enabled = true)?
	bool wants_subsonic_scan(const char* section);

	// Builds the event for a shot (begin for shot 1, shot otherwise). False if the weapon is not handled.
	bool make_shot_event(const SShotCtx& c, SCyclicEvent& out);

	// Event closing the burst of the weapon `weapon_id` (trigger released, reload, hide...). False if no burst runs.
	bool make_release_event(u16 weapon_id, bool ads, CObject* owner, int ai_type, float real_rpm, SCyclicEvent& out);

	// True while a burst of that weapon is open.
	bool session_active(u16 weapon_id);
	void end_session(u16 weapon_id);

	// Indoor state, pushed by the scripts (SAR) when its hysteresis flips.
	void set_indoor(bool v);
	bool get_indoor();
}
