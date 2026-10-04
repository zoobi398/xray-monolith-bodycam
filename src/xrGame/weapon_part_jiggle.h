#pragma once

#include "../xrEngine/bone.h"

struct attachable_hud_item;
class IKinematics;

// Secondary-motion ("jiggle") bone physics for weapon-attached moving parts (bipod legs, carry
// handles, keychains...). Same core idea as a damped-spring jiggle bone (see e.g. xray-monolith-lass's
// bone_spring.cpp, read for reference, not merged -- that fork targets the actor's own body skeleton on
// a different base build; this targets a weapon's own IKinematics instead, hooked through
// attachable_hud_item rather than CActor). Two differences from that reference design, both requested
// up front rather than added later:
//   1. Every tuning value lives PER BONE (in SJiggleBoneConfig), not as shared globals -- two bones on
//      the same weapon can have completely different physics.
//   2. Rotation is constrained to up to two explicitly configured hinge axes (pitch/roll), each with its
//      own independent gain and hard angle limit, instead of one isotropic swing cone -- this is what
//      actually keeps a part from swinging into geometry it shouldn't.
//
// Driving signal (v1): the weapon's own root transform (attachable_hud_item::m_item_transform, which by
// the time it's computed already includes Bodycam's recoil/sway/decompensation/mouse-aim -- see
// 00_SUIVI_CHANGEMENTS_ENGINE/07_ENGINE_CHANGES_GUN_PART_JIGGLE.md for how that was verified) is tracked
// frame to frame the same way xray-monolith-lass tracks the actor's world position: a pseudo-acceleration
// term in model space. No per-source (recoil vs sway vs sprint) wiring yet -- deliberately deferred until
// this is seen moving in-game; see the doc's "Known limitations" for the planned per-channel follow-up.

struct SJiggleAxisLimit
{
	bool active = false;
	Fvector axis = {0.f, 1.f, 0.f}; // bone-local, does not need to be normalized in the ltx
	float gain = 1.f;
	float max_deg = 10.f;
};

struct SJiggleBoneConfig
{
	shared_str section;	  // for debug logging only
	u16 bone_id = BI_NONE;

	float stiffness = 140.f;
	float damping = 11.f;
	float bone_length = 0.08f; // metres; converts lag distance -> swing angle, same role as in the reference design
	float world_gain = 0.6f;	// response to the weapon root's own acceleration (recoil kick, sprint bob, etc.)

	float translate_gain = 1.f;
	float translate_max = 0.015f; // metres; isotropic clamp on the translation component

	SJiggleAxisLimit pitch;
	SJiggleAxisLimit roll;
};

class CGunPartJiggleController;

struct SJiggleBoneState
{
	CGunPartJiggleController* owner = nullptr;
	SJiggleBoneConfig cfg;

	Fvector sim_pos = {0.f, 0.f, 0.f};	// simulated (lagging) position, bone-parent-local space
	Fvector velocity = {0.f, 0.f, 0.f}; // same space
	Fvector applied_lag = {0.f, 0.f, 0.f}; // this frame's clamped lag, cached so repeat CLBone calls re-apply it
	u32 last_frame = 0;
	bool primed = false;
};

// Owned by attachable_hud_item (one per equipped weapon/attached item). install()/remove() are safe to
// call even when the item has no jiggle_bones configured -- does nothing, costs nothing.
class CGunPartJiggleController
{
public:
	enum
	{
		MAX_JIGGLE_BONES = 4
	};

	CGunPartJiggleController();

	void install(attachable_hud_item* owner, const shared_str& weapon_section);
	void remove();

private:
	static void _BCL bone_callback(CBoneInstance* bi);

	void integrate(SJiggleBoneState& b, CBoneInstance* bi);
	void apply(SJiggleBoneState& b, CBoneInstance* bi);
	void update_world_accel();
	bool read_bone_config(const shared_str& section, SJiggleBoneConfig& out);

	attachable_hud_item* m_owner;
	IKinematics* m_model;

	SJiggleBoneState m_bones[MAX_JIGGLE_BONES];
	u16 m_bone_count;

	Fvector m_prev_world_pos;
	Fvector m_prev_world_vel;
	Fvector m_world_accel; // already rotated into the weapon's own model space
	u32 m_accel_frame;
	bool m_world_primed;
};
