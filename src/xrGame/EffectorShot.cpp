// EffectorShot.cpp: implementation of the CCameraShotEffector class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "EffectorShot.h"
#include "Weapon.h"

//-----------------------------------------------------------------------------
// Weapon shot effector
//-----------------------------------------------------------------------------

// calculate horz recoil independently of vert recoil
BOOL g_decouple_horz_recoil = FALSE;

// per-shot Msg() dump of the Insurgency recoil math (vert/horz kick, AR(1) state, lean rotation) for
// weapons with insurgency_recoil=1 -- off by default, toggle via the "Log Recoil Values" checkbox in
// the Bodycam Weapon Recoil MCM tab (or the g_insurgency_recoil_debug_log console command directly).
BOOL g_insurgency_recoil_debug_log = FALSE;

CWeaponShotEffector::CWeaponShotEffector()
{
	Reset();
	//	m_first_shot_pos = 0.0f;
}

void CWeaponShotEffector::Initialize(const CameraRecoil& cam_recoil)
{
	m_cam_recoil.Clone(cam_recoil);
	Reset();
}

void CWeaponShotEffector::UpdateCameraRecoil(const CameraRecoil& cam_recoil)
{
	m_cam_recoil.Clone(cam_recoil);
}

void CWeaponShotEffector::Reset()
{
	m_angle_vert = 0.0f;
	m_angle_horz = 0.0f;
	m_output_vert = 0.0f;
	m_output_horz = 0.0f;

	m_prev_angle_vert = 0.0f;
	m_prev_angle_horz = 0.0f;

	m_delta_vert = 0.0f;
	m_delta_horz = 0.0f;

	m_LastSeed = 0;
	m_shot_numer = 0;
	m_single_shot = false;
	m_first_shot = false;
	m_actived = false;
	m_shot_end = true;
	m_last_horz_step = 0.0f;
	m_last_shot_impulse = 0.0f;
}

void CWeaponShotEffector::Shot(CWeapon* weapon, float actor_roll)
{
	R_ASSERT(weapon);
	m_shot_numer = weapon->ShotsFired() - 1;
	if (m_shot_numer <= 0)
	{
		// m_shot_numer = 0; // it's now done in Reset()
		Reset();
	}
	m_single_shot = (weapon->GetCurrentFireMode() == 1);

	CCartridge* ammo = !weapon->m_magazine.empty() ? &weapon->m_magazine.back() : (0);
	float k_cam_disp = ammo ? ammo->param_s.k_cam_dispersion : 1.0f;
	float angle = m_cam_recoil.Dispersion
		* weapon->cur_silencer_koef.cam_dispersion
		* weapon->cur_scope_koef.cam_dispersion
		* weapon->cur_launcher_koef.cam_dispersion
		* k_cam_disp;
	angle += m_cam_recoil.DispersionInc
		* weapon->cur_silencer_koef.cam_disper_inc
		* weapon->cur_scope_koef.cam_disper_inc
		* weapon->cur_launcher_koef.cam_disper_inc
		* (float)m_shot_numer;
	Shot2(angle, actor_roll);
}

void CWeaponShotEffector::Shot2(float angle, float actor_roll)
{
	float dvert = angle * (m_cam_recoil.DispersionFrac + m_Random.randF(-1.0f, 1.0f) * (1.0f - m_cam_recoil.
		DispersionFrac));

	if (m_cam_recoil.InsurgencyRecoil)
	{
		// ---- Insurgency-style path ----
		// AR(1) horizontal step with a CONSTANT amplitude envelope (cam_step_angle_horz), deliberately
		// NOT scaled by the accumulated vertical angle. Measured on real Insurgency footage: |yaw_step|
		// correlates ~0 (even slightly negative) with cam_pitch_settled across a burst -- the stock
		// coupled formula (step *= accumulated_vert/MaxAngleVert) instead makes the horizontal step grow
		// ~3-4x from the start to the end of a sustained burst, which is a 2008 STALKER design choice
		// with no support in the source footage. YawRho == 0 reduces this to a fresh random draw each
		// shot, still at constant amplitude (not the stock coupled shape).
		float rdm = m_Random.randF(-1.0f, 1.0f);
		float base_step = m_cam_recoil.StepAngleHorz;
		float dhorz = m_cam_recoil.YawRho * m_last_horz_step + (1.0f - m_cam_recoil.YawRho) * rdm * base_step;
		m_last_horz_step = dhorz;

		// Rotation angle from lean coupling, computed unconditionally (theta is 0 and the rotation a
		// no-op whenever LeanCoupling is 0) so the debug log below always has a real value to show.
		float theta = actor_roll * m_cam_recoil.LeanCoupling;
		if (!fis_zero(m_cam_recoil.LeanCoupling))
		{
			// Rotate the (vertical, horizontal) impulse this shot contributes by the actor's
			// current roll (lean) scaled by a tunable gain -- reproduces the yaw bias measured
			// on Insurgency footage when leaning, without hard-coding either engine's lean angle.
			float c = cosf(theta), s = sinf(theta);
			float ndvert = dvert * c - dhorz * s;
			dhorz = dvert * s + dhorz * c;
			dvert = ndvert;
		}

		m_angle_vert += dvert;
		clamp(m_angle_vert, -m_cam_recoil.MaxAngleVert, m_cam_recoil.MaxAngleVert);
		if (fis_zero(m_angle_vert - m_cam_recoil.MaxAngleVert))
		{
			m_angle_vert *= m_Random.randF(0.96f, 1.04f);
		}

		m_angle_horz += dhorz;
		clamp(m_angle_horz, -m_cam_recoil.MaxAngleHorz, m_cam_recoil.MaxAngleHorz);

		if (g_insurgency_recoil_debug_log)
		{
			Msg("* insurgency recoil shot=%d | vert: dvert=%.4f angle_vert=%.4f | horz: rdm=%.3f step=%.4f rho=%.2f dhorz=%.4f angle_horz=%.4f | lean: roll=%.4f gain=%.2f theta=%.4f | return_mode=%d relax_speed=%.4f",
				m_shot_numer, dvert, m_angle_vert, rdm, base_step, m_cam_recoil.YawRho, dhorz, m_angle_horz,
				actor_roll, m_cam_recoil.LeanCoupling, theta, m_cam_recoil.ReturnMode ? 1 : 0, m_cam_recoil.RelaxSpeed);
		}
	}
	else
	{
		// ---- stock path, byte-identical to the original code ----
		m_angle_vert += dvert;
		clamp(m_angle_vert, -m_cam_recoil.MaxAngleVert, m_cam_recoil.MaxAngleVert);
		if (fis_zero(m_angle_vert - m_cam_recoil.MaxAngleVert))
		{
			m_angle_vert *= m_Random.randF(0.96f, 1.04f);
		}

		float rdm = m_Random.randF(-1.0f, 1.0f);
		if (g_decouple_horz_recoil)
		{
			m_angle_horz += rdm * m_cam_recoil.StepAngleHorz;
		}
		else
		{
			m_angle_horz += (m_angle_vert / m_cam_recoil.MaxAngleVert) * rdm * m_cam_recoil.StepAngleHorz;
		}
		clamp(m_angle_horz, -m_cam_recoil.MaxAngleHorz, m_cam_recoil.MaxAngleHorz);
	}

	m_last_shot_impulse = _abs(dvert);
	m_first_shot = true;
	m_actived = true;
	m_shot_end = false;
}

void CWeaponShotEffector::Relax()
{
	float time_to_relax = _abs(m_angle_vert) / m_cam_recoil.RelaxSpeed;
	float relax_speed_horz = (fis_zero(time_to_relax)) ? 0.0f : _abs(m_angle_horz) / time_to_relax;

	float dt = Device.fTimeDelta;

	if (m_angle_horz >= 0.0f) // h
	{
		m_angle_horz -= relax_speed_horz * dt;
	}
	else
	{
		m_angle_horz += relax_speed_horz * dt;
	}

	if (m_angle_vert >= 0.0f) // v
	{
		m_angle_vert -= m_cam_recoil.RelaxSpeed * dt;
		if (m_angle_vert < 0.0f)
		{
			m_angle_vert = 0.0f;
			m_actived = false;
		}
	}
	else
	{
		m_angle_vert += m_cam_recoil.RelaxSpeed * dt;
		if (m_angle_vert > 0.0f)
		{
			m_angle_vert = 0.0f;
			m_actived = false;
		}
	}

	if (g_insurgency_recoil_debug_log && m_cam_recoil.InsurgencyRecoil)
	{
		Msg("* insurgency relax angle_vert=%.4f angle_horz=%.4f relax_speed_horz=%.4f actived=%d",
			m_angle_vert, m_angle_horz, relax_speed_horz, m_actived ? 1 : 0);
	}
}

namespace
{
// Eases 'out' toward 'target' over rise_time_ms whenever |target| is growing (a fresh kick); tracks
// 'target' exactly, instantly, whenever it's shrinking (Relax() already computed the correct,
// measured-linear shape there -- this must not smooth that). rise_time_ms <= 0 means instant both ways.
void ApproachRise(float& out, float target, float rise_time_ms, float dt)
{
	if (rise_time_ms <= 0.0f || _abs(target) <= _abs(out))
	{
		out = target;
		return;
	}
	float rate = 1000.0f / rise_time_ms;
	float t = 1.0f - expf(-rate * dt);
	out += (target - out) * t;
}
}

void CWeaponShotEffector::Update()
{
	if (m_actived && m_cam_recoil.ReturnMode /*|| m_single_shot*/)
	{
		Relax();
	}

	if (!m_cam_recoil.ReturnMode && m_shot_end && !m_single_shot)
	{
		m_actived = false;
	}

	float dt = Device.fTimeDelta;

	// Continuous horizontal center-pull, independent of cam_return/Relax() above (which stays exactly
	// as configured -- vertical included -- so a weapon tuned for "holds until you correct it" keeps
	// that feel). Without this, m_angle_horz is an unconstrained random walk during a sustained burst:
	// per the arcsine law, a driftless random walk spends most of its time on whichever side it commits
	// to early rather than crossing back through center, and insurgency_yaw_rho's memory only reinforces
	// that. This pulls it back exponentially at a tunable rate; 0 = off, today's behaviour unchanged.
	// Also decays m_last_horz_step (the yaw_rho AR(1) memory state) by the same factor -- without this,
	// the memory keeps "reloading" a step in whatever direction it last leaned every subsequent shot,
	// fighting the pull on m_angle_horz itself and making late-burst recovery far weaker than the pull
	// rate alone would suggest (diagnosed 17/09 from the bias concentrating in a magazine's last third).
	if (m_cam_recoil.InsurgencyRecoil && m_cam_recoil.YawCenterPull > 0.0f)
	{
		const float pull_factor = expf(-m_cam_recoil.YawCenterPull * dt);
		m_angle_horz *= pull_factor;
		m_last_horz_step *= pull_factor;
	}

	// Rise-time smoothing only makes sense for the vertical kick, which climbs monotonically within a
	// burst (a real "fresh kick eases in" shape). Horizontal is a noisy, direction-reversing random walk
	// (insurgency_yaw_rho) -- applying the same smoothing to it means, at full-auto cadence (shots closer
	// together than rise_time_ms), the output permanently lags the true value while it's growing, then
	// instantly snaps low the moment a shot nudges the magnitude down (ApproachRise's "instant while
	// shrinking" rule) -- a lag-then-snap ratchet that reads as a sticky, one-sided drift and never shows
	// up at semi-auto pace, where each shot's rise time fully settles before the next one lands (16/09,
	// diagnosed from exactly that semi-vs-auto difference). Horizontal is therefore always instant here,
	// matching cam_step_angle_horz's existing "constant amplitude, not coupled to burst progression" design.
	if (m_cam_recoil.InsurgencyRecoil && m_cam_recoil.RiseTimeMs > 0.0f)
	{
		ApproachRise(m_output_vert, m_angle_vert, m_cam_recoil.RiseTimeMs, dt);
		m_output_horz = m_angle_horz;
	}
	else
	{
		m_output_vert = m_angle_vert;
		m_output_horz = m_angle_horz;
	}

	m_delta_vert = m_output_vert - m_prev_angle_vert;
	m_delta_horz = m_output_horz - m_prev_angle_horz;

	m_prev_angle_vert = m_output_vert;
	m_prev_angle_horz = m_output_horz;

	//	Msg( " <<[%d]  v=%.4f  dv=%.4f   a=%d s=%d  fr=%d", m_shot_numer, m_angle_vert, m_delta_vert, m_actived, m_first_shot, Device.dwFrame );
}

void CWeaponShotEffector::GetDeltaAngle(Fvector& angle)
{
	angle.x = -m_output_vert;
	angle.y = -m_output_horz;
	angle.z = 0.0f;
}

void CWeaponShotEffector::GetLastDelta(Fvector& delta_angle)
{
	delta_angle.x = -m_delta_vert;
	delta_angle.y = -m_delta_horz;
	delta_angle.z = 0.0f;
}

void CWeaponShotEffector::SetRndSeed(s32 Seed)
{
	if (m_LastSeed == 0)
	{
		m_LastSeed = Seed;
		//		m_Random.seed		(Seed);
		m_Random.seed(Device.dwFrame);
	}
}

void CWeaponShotEffector::ChangeHP(float* pitch, float* yaw)
{
	*pitch -= m_delta_vert; // y = pitch = p = vert
	*yaw -= m_delta_horz; // x = yaw   = h = horz

	//	if ( m_first_shot )
	//	{
	//		m_first_shot_pos = *pitch;
	//		m_first_shot = false;
	//	}

	//	if ( m_cam_recoil.ReturnMode && m_cam_recoil.StopReturn && (*pitch > m_first_shot_pos + 0.1f) )
	//	{
	//		m_actived = false;
	//	}
	//	Msg( "[%d]  pitch = %.4f   yaw = %.4f    fs=%d    a=%d  fr=%d", m_shot_numer, *pitch, *yaw, m_first_shot, m_actived, Device.dwFrame );
}

//-----------------------------------------------------------------------------
// Camera shot effector
//-----------------------------------------------------------------------------

CCameraShotEffector::CCameraShotEffector(const CameraRecoil& cam_recoil)
	: CEffectorCam(eCEShot, 100000.0f)
{
	CWeaponShotEffector::Initialize(cam_recoil);
	m_pActor = NULL;
	m_WeaponID = (u16)-1;
}

CCameraShotEffector::~CCameraShotEffector()
{
}

BOOL CCameraShotEffector::ProcessCam(SCamEffectorInfo& info)
{
	Update();
	return TRUE;
}
