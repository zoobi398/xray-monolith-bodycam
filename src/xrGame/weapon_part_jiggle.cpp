#include "stdafx.h"
#include "weapon_part_jiggle.h"
#include "player_hud.h"
#include "../Include/xrRender/Kinematics.h"

// Live kill switch / debug log, console-bound (console_commands.cpp). Per-bone tuning itself is
// config-driven (see read_bone_config) -- these two are the only globals, matching the project's
// established pattern of a live-tunable escape hatch alongside config-driven values.
BOOL g_gunjiggle_enabled = TRUE;
BOOL g_gunjiggle_debug = FALSE;

static const float JIGGLE_MAX_DT = 0.05f;
static const float JIGGLE_SUBSTEP = 1.0f / 120.0f;
static const int JIGGLE_MAX_SUBSTEPS = 8;
static const float JIGGLE_SNAP_DIST = 0.5f;
static const float JIGGLE_MAX_WORLD_ACCEL = 80.0f;

CGunPartJiggleController::CGunPartJiggleController()
	: m_owner(nullptr), m_model(nullptr), m_bone_count(0), m_accel_frame(0), m_world_primed(false)
{
	m_prev_world_pos.set(0.f, 0.f, 0.f);
	m_prev_world_vel.set(0.f, 0.f, 0.f);
	m_world_accel.set(0.f, 0.f, 0.f);
}

bool CGunPartJiggleController::read_bone_config(const shared_str& section, SJiggleBoneConfig& out)
{
	if (!pSettings->section_exist(section))
		return false;

	out.section = section;

	if (pSettings->line_exist(section, "jiggle_stiffness"))
		out.stiffness = pSettings->r_float(section, "jiggle_stiffness");
	if (pSettings->line_exist(section, "jiggle_damping"))
		out.damping = pSettings->r_float(section, "jiggle_damping");
	if (pSettings->line_exist(section, "jiggle_bone_length"))
		out.bone_length = pSettings->r_float(section, "jiggle_bone_length");
	if (pSettings->line_exist(section, "jiggle_world_gain"))
		out.world_gain = pSettings->r_float(section, "jiggle_world_gain");
	if (pSettings->line_exist(section, "jiggle_translate_gain"))
		out.translate_gain = pSettings->r_float(section, "jiggle_translate_gain");
	if (pSettings->line_exist(section, "jiggle_translate_max"))
		out.translate_max = pSettings->r_float(section, "jiggle_translate_max");

	if (pSettings->line_exist(section, "jiggle_pitch_max_deg"))
	{
		out.pitch.active = true;
		out.pitch.max_deg = pSettings->r_float(section, "jiggle_pitch_max_deg");
		if (pSettings->line_exist(section, "jiggle_pitch_axis"))
			out.pitch.axis = pSettings->r_fvector3(section, "jiggle_pitch_axis");
		if (pSettings->line_exist(section, "jiggle_pitch_gain"))
			out.pitch.gain = pSettings->r_float(section, "jiggle_pitch_gain");
	}

	if (pSettings->line_exist(section, "jiggle_roll_max_deg"))
	{
		out.roll.active = true;
		out.roll.max_deg = pSettings->r_float(section, "jiggle_roll_max_deg");
		if (pSettings->line_exist(section, "jiggle_roll_axis"))
			out.roll.axis = pSettings->r_fvector3(section, "jiggle_roll_axis");
		if (pSettings->line_exist(section, "jiggle_roll_gain"))
			out.roll.gain = pSettings->r_float(section, "jiggle_roll_gain");
	}

	out.stiffness = _max(out.stiffness, 0.f);
	out.damping = _max(out.damping, 0.f);
	out.bone_length = _max(out.bone_length, EPS_L);
	out.translate_max = _max(out.translate_max, 0.f);
	if (out.pitch.axis.magnitude() > EPS_L)
		out.pitch.axis.normalize();
	else
		out.pitch.active = false;
	if (out.roll.axis.magnitude() > EPS_L)
		out.roll.axis.normalize();
	else
		out.roll.active = false;

	return true;
}

void CGunPartJiggleController::install(attachable_hud_item* owner, const shared_str& weapon_section)
{
	remove();

	if (!owner || !owner->m_model)
		return;

	if (!pSettings->line_exist(weapon_section, "jiggle_bones"))
		return;

	IKinematics* K = owner->m_model;
	LPCSTR list = pSettings->r_string(weapon_section, "jiggle_bones");

	const int count = _GetItemCount(list);
	for (int i = 0; i < count && m_bone_count < MAX_JIGGLE_BONES; ++i)
	{
		string256 token;
		_GetItem(list, i, token);

		LPSTR colon = strchr(token, ':');
		if (!colon)
		{
			Msg("! [gun_jiggle] %s: jiggle_bones entry '%s' has no ':section' part, skipping", weapon_section.c_str(), token);
			continue;
		}
		*colon = 0;
		LPCSTR bone_name = token;
		LPCSTR cfg_section = colon + 1;

		const u16 bid = K->LL_BoneID(bone_name);
		if (bid == BI_NONE)
		{
			Msg("! [gun_jiggle] %s: bone [%s] not found on this model, skipping", weapon_section.c_str(), bone_name);
			continue;
		}

		CBoneInstance& bi = K->LL_GetBoneInstance(bid);
		if (bi.callback())
		{
			Msg("! [gun_jiggle] %s: bone [%s] already has a callback, skipping", weapon_section.c_str(), bone_name);
			continue;
		}

		SJiggleBoneConfig cfg;
		if (!read_bone_config(cfg_section, cfg))
		{
			Msg("! [gun_jiggle] %s: section [%s] for bone [%s] does not exist, skipping", weapon_section.c_str(), cfg_section,
				bone_name);
			continue;
		}
		cfg.bone_id = bid;

		SJiggleBoneState& b = m_bones[m_bone_count];
		b.owner = this;
		b.cfg = cfg;
		b.sim_pos.set(0.f, 0.f, 0.f);
		b.velocity.set(0.f, 0.f, 0.f);
		b.applied_lag.set(0.f, 0.f, 0.f);
		b.last_frame = 0;
		b.primed = false;

		++m_bone_count;
	}

	if (!m_bone_count)
		return;

	// Installed only once the array is fully built, so the pointers handed to the callbacks stay valid
	// for the lifetime of this controller (m_bones never resizes after this point).
	for (u16 i = 0; i < m_bone_count; ++i)
		K->LL_GetBoneInstance(m_bones[i].cfg.bone_id).set_callback(bctCustom, bone_callback, &m_bones[i], FALSE);

	m_owner = owner;
	m_model = K;
	m_world_primed = false;
	m_accel_frame = 0;

	if (g_gunjiggle_debug)
		Msg("* [gun_jiggle] %s: installed on %d bone(s)", weapon_section.c_str(), m_bone_count);
}

void CGunPartJiggleController::remove()
{
	if (m_model)
	{
		for (u16 i = 0; i < m_bone_count; ++i)
		{
			CBoneInstance& bi = m_model->LL_GetBoneInstance(m_bones[i].cfg.bone_id);
			if (bi.callback() == bone_callback && bi.callback_param() == &m_bones[i])
				bi.reset_callback();
		}
	}

	m_model = nullptr;
	m_owner = nullptr;
	m_bone_count = 0;
	m_world_primed = false;
	m_accel_frame = 0;
}

// Pseudo-force from the weapon's own root motion (attachable_hud_item::m_item_transform), tracked the
// same way xray-monolith-lass tracks the actor's world position for its jump/landing response -- except
// here the root already carries Bodycam's full recoil/sway/decompensation/mouse-aim composite (verified
// by reading player_hud.cpp: the Bodycam hud offset is folded into the hands' root transform before
// CalculateBones runs, and the weapon's own m_item_transform is derived from the resulting hand-bone
// pose), so no separate per-source wiring is needed for v1 -- this one signal already reflects all of it.
void CGunPartJiggleController::update_world_accel()
{
	if (m_accel_frame == Device.dwFrame)
		return;
	m_accel_frame = Device.dwFrame;

	if (!m_owner)
	{
		m_world_accel.set(0.f, 0.f, 0.f);
		return;
	}

	const Fmatrix& X = m_owner->m_item_transform;
	const float dt = _max(Device.fTimeDelta, EPS_S);

	if (!m_world_primed)
	{
		m_prev_world_pos.set(X.c);
		m_prev_world_vel.set(0.f, 0.f, 0.f);
		m_world_accel.set(0.f, 0.f, 0.f);
		m_world_primed = true;
		return;
	}

	Fvector d;
	d.sub(X.c, m_prev_world_pos);
	m_prev_world_pos.set(X.c);

	if (d.magnitude() > JIGGLE_SNAP_DIST)
	{
		m_prev_world_vel.set(0.f, 0.f, 0.f);
		m_world_accel.set(0.f, 0.f, 0.f);
		return;
	}

	Fvector vel;
	vel.set(d).mul(1.f / dt);

	Fvector accel;
	accel.sub(vel, m_prev_world_vel).mul(1.f / dt);
	m_prev_world_vel.set(vel);

	const float a = accel.magnitude();
	if (a > JIGGLE_MAX_WORLD_ACCEL)
		accel.mul(JIGGLE_MAX_WORLD_ACCEL / a);

	// world -> weapon-root-local; X is orthonormal, so the inverse rotation is the transpose.
	m_world_accel.set(accel.dotproduct(X.i), accel.dotproduct(X.j), accel.dotproduct(X.k));
}

void CGunPartJiggleController::integrate(SJiggleBoneState& b, CBoneInstance* bi)
{
	const Fvector target = bi->mTransform.c;

	if (!b.primed)
	{
		b.sim_pos.set(target);
		b.velocity.set(0.f, 0.f, 0.f);
		b.applied_lag.set(0.f, 0.f, 0.f);
		b.primed = true;
		return;
	}

	Fvector delta;
	delta.sub(target, b.sim_pos);
	if (delta.magnitude() > JIGGLE_SNAP_DIST)
	{
		b.sim_pos.set(target);
		b.velocity.set(0.f, 0.f, 0.f);
		b.applied_lag.set(0.f, 0.f, 0.f);
		return;
	}

	update_world_accel();

	float dt = Device.fTimeDelta;
	clamp(dt, 0.f, JIGGLE_MAX_DT);
	if (dt <= 0.f)
		return;

	int steps = int(ceilf(dt / JIGGLE_SUBSTEP));
	clamp(steps, 1, JIGGLE_MAX_SUBSTEPS);
	const float h = dt / float(steps);

	Fvector world_force;
	world_force.set(m_world_accel).mul(b.cfg.world_gain);

	for (int s = 0; s < steps; ++s)
	{
		Fvector accel;
		accel.sub(target, b.sim_pos).mul(b.cfg.stiffness);
		accel.mad(b.velocity, -b.cfg.damping);
		accel.sub(world_force);

		b.velocity.mad(accel, h);
		b.sim_pos.mad(b.velocity, h);
	}

	Fvector lag;
	lag.sub(b.sim_pos, target);

	// Translation gets its own isotropic clamp (what actually reaches M.c); the clamp used for the
	// rotation calculation below is intentionally generous (JIGGLE_SNAP_DIST-scale) since the real limit
	// for rotation is each configured axis's own max_deg, applied in apply() -- clamping the raw lag here
	// too would just make both channels fight over the same budget for no reason.
	b.applied_lag.set(lag);
}

void CGunPartJiggleController::apply(SJiggleBoneState& b, CBoneInstance* bi)
{
	const SJiggleBoneConfig& cfg = b.cfg;
	Fmatrix& M = bi->mTransform;

	const float lag_len = b.applied_lag.magnitude();
	if (lag_len < EPS_L)
		return;

	// Bone-local lag: three dot products against the (orthonormal) bone matrix.
	Fvector local;
	local.set(b.applied_lag.dotproduct(M.i), b.applied_lag.dotproduct(M.j), b.applied_lag.dotproduct(M.k));

	// Each configured hinge rotates the bone's own +Z (its "pointing" direction) toward the lag
	// direction; the angle about a given hinge axis is that swing's component along that axis --
	// cross(forward, local) is the *unconstrained* swing axis (same quantity the isotropic-cone design
	// uses as its rotation axis directly), projecting it onto one fixed, configured axis is what turns
	// that free cone into a single hinge with its own independent limit.
	static const Fvector FORWARD = {0.f, 0.f, 1.f};
	Fvector swing;
	swing.crossproduct(FORWARD, local);

	Fmatrix total_rot;
	total_rot.identity();
	bool any_rot = false;

	if (cfg.pitch.active && !fis_zero(cfg.pitch.gain))
	{
		const float signed_len = swing.dotproduct(cfg.pitch.axis);
		float angle = atanf(_abs(signed_len) / cfg.bone_length) * cfg.pitch.gain;
		clamp(angle, 0.f, deg2rad(cfg.pitch.max_deg));
		if (signed_len < 0.f)
			angle = -angle;
		if (!fis_zero(angle))
		{
			Fmatrix R;
			R.rotation(cfg.pitch.axis, angle);
			total_rot.mulA_43(R);
			any_rot = true;
		}
	}

	if (cfg.roll.active && !fis_zero(cfg.roll.gain))
	{
		const float signed_len = swing.dotproduct(cfg.roll.axis);
		float angle = atanf(_abs(signed_len) / cfg.bone_length) * cfg.roll.gain;
		clamp(angle, 0.f, deg2rad(cfg.roll.max_deg));
		if (signed_len < 0.f)
			angle = -angle;
		if (!fis_zero(angle))
		{
			Fmatrix R;
			R.rotation(cfg.roll.axis, angle);
			total_rot.mulA_43(R);
			any_rot = true;
		}
	}

	if (any_rot)
	{
		// mul_43(A, B) applies B first then A -- this rotates within the bone's own frame about its own
		// origin (M.c untouched), same composition rule the reference design uses.
		Fmatrix out;
		out.mul_43(M, total_rot);
		M.set(out);
	}

	if (!fis_zero(cfg.translate_gain))
	{
		Fvector t = b.applied_lag;
		t.mul(cfg.translate_gain);
		const float t_len = t.magnitude();
		if (t_len > cfg.translate_max)
			t.mul(cfg.translate_max / t_len);
		M.c.add(t);
	}
}

void _BCL CGunPartJiggleController::bone_callback(CBoneInstance* bi)
{
	SJiggleBoneState* b = static_cast<SJiggleBoneState*>(bi->callback_param());
	if (!b || !b->owner || !g_gunjiggle_enabled)
		return;

	// CKinematics::CLBone can run more than once per frame for the same visual (e.g. a shadow pass) --
	// integrate on the first call of the frame only, then re-apply the cached displacement on any later
	// call, since BuildBoneMatrix rebuilds the animated pose from scratch every time.
	if (b->last_frame != Device.dwFrame && !Device.Paused())
	{
		b->last_frame = Device.dwFrame;
		b->owner->integrate(*b, bi);
	}

	b->owner->apply(*b, bi);
}
