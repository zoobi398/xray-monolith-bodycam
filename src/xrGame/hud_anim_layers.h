#pragma once
// Independent animation layers for HUD weapons ("calques d'animation HUD").
// Design, config syntax and animator guide: 00_SUIVI_CHANGEMENTS_ENGINE/09_THEORIE_CALQUES_ANIMATION_HUD.md
//
// A layer is a motion of the weapon's own OMF played on an ADDITIVE channel (2 or 3) of the weapon model, started by an
// event (shot, burst start, end of burst, HUD animation name, motion mark, script call). Channels 2 and 3 are never cut by
// the main animation (channel 0), so a layer runs to the end of its own duration. Opt-in per weapon through the
// "hud_layers" key of the HUD section; an item without that key costs nothing (install() returns at once, active() is false).

#include "../Include/xrRender/KinematicsAnimated.h"

struct attachable_hud_item;

enum EHudLayerRetrigger
{
	hlr_crossfade = 0, // new instance from frame 0, the old one fades out (default)
	hlr_restart,       // the running instance goes back to frame 0
	hlr_ignore,        // nothing happens while an instance is still playing
};

class CHudAnimLayers
{
public:
	enum
	{
		MAX_LAYERS = 8,
		MAX_PARTS_USED = 4,    // same value as MAX_PARTS of the render (bone partitions per model)
		MAX_INSTANCES = 6,
	};

	CHudAnimLayers();

	void install(attachable_hud_item* owner, const shared_str& hud_section);
	void remove();
	bool active() const { return m_count > 0; }

	// events (all no-ops unless the item declared hud_layers)
	void on_shot(int shot_no);
	void on_fire_end();
	void on_anim(const shared_str& alias);
	void on_mark(const shared_str& mark);
	bool play_by_name(const char* layer_section);

private:
	struct SInstance
	{
		CBlend* blend[MAX_PARTS_USED];
		u8 count;
		MotionID mid;
	};

	struct SLayer
	{
		shared_str section;
		MotionID motions[8];
		u8 motion_count = 0;
		u8 last_variant = 0xff;
		u8 channel = 3;
		EHudLayerRetrigger retrigger = hlr_crossfade;
		bool on_shot = false, on_burst_start = false, on_fire_end = false;
		xr_vector<shared_str> anim_patterns;
		xr_vector<shared_str> mark_names;
		float speed = 1.f;
		bool speed_rpm = false;
		float blend_in = 0.03f;
		float blend_out = 0.10f;
		float start_min = 0.f, start_max = 0.f;
		int part = -1; // -1 = all partitions
		u8 max_instances = 3;
		SInstance inst[MAX_INSTANCES];
		u8 inst_count = 0;
	};

	void fire(SLayer& L);
	bool read_layer(const shared_str& hud_section, const shared_str& layer_section, SLayer& out);
	static bool blend_alive(const CBlend* b, const MotionID& mid, u8 channel);
	float weapon_rpm() const;

	attachable_hud_item* m_owner;
	IKinematicsAnimated* m_ka;
	SLayer m_layers[MAX_LAYERS];
	u8 m_count;
	bool m_burst_active;
};
