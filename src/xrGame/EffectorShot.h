// EffectorShot.h: interface for the CCameraShotEffector class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "CameraEffector.h"
#include "../xrEngine/cameramanager.h"
#include "Actor.h"
#include "CameraRecoil.h"

class CWeapon;

class CWeaponShotEffector
{
protected:
	CameraRecoil m_cam_recoil;

	float m_angle_vert;
	float m_angle_horz;

	float m_prev_angle_vert;
	float m_prev_angle_horz;

	float m_delta_vert;
	float m_delta_horz;

	int m_shot_numer;
	bool m_shot_end;
	bool m_first_shot;
	//	float			m_first_shot_pos;

	bool m_actived;
	bool m_single_shot;

	// Insurgency-style recoil: AR(1) state for the horizontal step (CameraRecoil::YawRho).
	// Unused (stays 0) unless CameraRecoil::InsurgencyRecoil is set.
	float m_last_horz_step;

	// Magnitude of the vertical delta this shot actually applied (post lean-rotation), so callers
	// (Bodycam's cosmetic viewmodel kick) can scale their own impulse to the real recoil instead of
	// using a fixed constant. Set every shot regardless of InsurgencyRecoil.
	float m_last_shot_impulse;

	// Insurgency-style recoil: camera-facing output, eased toward m_angle_vert/horz over
	// CameraRecoil::RiseTimeMs on the way up (a fresh kick), tracked instantly on the way down (the
	// already-validated linear Relax() shape, untouched). Equals m_angle_vert/horz exactly whenever
	// RiseTimeMs is 0 or InsurgencyRecoil is off -- ChangeHP()/GetDeltaAngle() then see identical
	// values to before this was added.
	float m_output_vert;
	float m_output_horz;

private:
	CRandom m_Random;
	s32 m_LastSeed;

public:
	CWeaponShotEffector();

	virtual ~CWeaponShotEffector()
	{
	};

	void Initialize(const CameraRecoil& cam_recoil);
	void UpdateCameraRecoil(const CameraRecoil& cam_recoil);
	void Reset();

	IC bool IsActive() { return m_actived; }
	//		void	SetActive			(bool Active)		{			m_actived = Active;		}
	IC void StopShoting() { m_shot_end = true; }

	void Update();

	void SetRndSeed(s32 Seed);

	void Shot(CWeapon* weapon, float actor_roll = 0.f);
	void Shot2(float angle, float actor_roll = 0.f);

	void GetDeltaAngle(Fvector& angle);
	void GetLastDelta(Fvector& delta_angle);
	void ChangeHP(float* pitch, float* yaw);
	IC float GetLastShotImpulse() { return m_last_shot_impulse; }

	// Camera-facing accumulated recoil (post rise-time easing), for Bodycam's viewmodel-follow
	// channel -- lets the cosmetic viewmodel track the real recoil climb instead of only reacting to
	// per-shot impulses. Only meaningful (non-zero) while InsurgencyRecoil is set; stock weapons never
	// feed this into anything, see ActorCameras.cpp::cam_BodycamVisualUpdate.
	IC float GetOutputVert() const { return m_output_vert; }
	IC float GetOutputHorz() const { return m_output_horz; }
	IC bool IsInsurgencyRecoil() const { return m_cam_recoil.InsurgencyRecoil; }
	IC float GetYawCenterPull() const { return m_cam_recoil.YawCenterPull; }
	IC float GetMuzzlePivot() const { return m_cam_recoil.MuzzlePivot; }
	IC float GetDecompScale() const { return m_cam_recoil.DecompScale; }
	IC int GetDecompMinShots() const { return m_cam_recoil.DecompMinShots; }
	IC float GetDecompImpulse() const { return m_cam_recoil.DecompImpulse; }
	IC float GetDecompVerticalScale() const { return m_cam_recoil.DecompVerticalScale; }
	IC float GetDecompForwardScale() const { return m_cam_recoil.DecompForwardScale; }
	IC float GetDecompPitchScale() const { return m_cam_recoil.DecompPitchScale; }
	IC float GetDecompHorizontalScale() const { return m_cam_recoil.DecompHorizontalScale; }
	IC float GetDecompAdsScale() const { return m_cam_recoil.DecompAdsScale; }

	// 0-based index of the last shot fired (weapon->ShotsFired() - 1, i.e. WeaponMagazined::m_iShotNum
	// - 1) -- set every Shot() call, NOT cleared by StopShoting(), so it still holds the just-ended
	// burst's shot count when on_weapon_shot_stop() reads it right after FireEnd().
	IC int GetShotNumber() const { return m_shot_numer; }

protected:
	void Relax();
};

class CCameraShotEffector : public CWeaponShotEffector, public CEffectorCam
{
protected:
	CActor* m_pActor;
public:
	//-					CCameraShotEffector	(float max_angle, float relax_speed, float max_angle_horz, float step_angle_horz, float angle_frac);
	CCameraShotEffector(const CameraRecoil& cam_recoil);
	virtual ~CCameraShotEffector();

	virtual BOOL ProcessCam(SCamEffectorInfo& info);
	virtual void SetActor(CActor* pActor) { m_pActor = pActor; };

	virtual CCameraShotEffector* cast_effector_shot() { return this; }
	u16 m_WeaponID;
};
