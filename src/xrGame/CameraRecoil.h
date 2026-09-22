////////////////////////////////////////////////////////////////////////////
//	Module 		: CameraRecoil.h
//	Created 	: 26.05.2008
//	Author		: Evgeniy Sokolov
//	Description : Camera Recoil struct
////////////////////////////////////////////////////////////////////////////

#ifndef CAMERA_RECOIL_H_INCLUDED
#define CAMERA_RECOIL_H_INCLUDED

//отдача при стрельбе 
struct CameraRecoil
{
	float RelaxSpeed;
	float RelaxSpeed_AI;
	float Dispersion;
	float DispersionInc;
	float DispersionFrac;
	float MaxAngleVert;
	float MaxAngleHorz;
	float StepAngleHorz;
	bool ReturnMode;
	bool StopReturn;

	// Insurgency-style recoil, opt-in per weapon (see EffectorShot.cpp). All default to the
	// values below, which reproduce the exact stock behaviour above for any weapon that never
	// sets the corresponding .ltx keys.
	bool InsurgencyRecoil;   // opt-in switch; false = CWeaponShotEffector::Shot2 stock path, untouched
	float YawRho;            // AR(1) coefficient on the horizontal step; 0 = stock memoryless step
	float LeanCoupling;      // gain on the actor's current roll when rotating the shot impulse; 0 = no lean bias
	float RiseTimeMs;        // ms for the camera to ease into a fresh kick; 0 = instant (stock shape)
	float MuzzlePivot;       // 0-1+: how much of Bodycam's vertical viewmodel rotation redirects around a
	                         // muzzle-heavy pivot instead of rotating the whole viewmodel as one rigid
	                         // block; 0 = current rigid behaviour, unchanged. See bodycam_simulation.cpp.
	float YawCenterPull;     // 1/s: continuous exponential pull of the horizontal recoil angle back toward
	                         // 0, independent of cam_return/Relax() (which stays exactly as configured,
	                         // vertical included). 0 = no pull, today's behaviour: an unconstrained random
	                         // walk that (per the arcsine law) tends to camp on whichever side it commits
	                         // to early in a sustained burst instead of crossing back through center.
	float DecompScale;       // Per-weapon multiplier (default 1) on top of the global Bodycam Weapon
	                         // Recoil "Decompensation" sliders -- GetLastShotImpulse() already scales
	                         // automatically with this weapon's own cam_dispersion_inc/cam_max_angle, so
	                         // 1 is "just use that", this is only for dialling a specific weapon up/down
	                         // (or to exactly 0 to disable) beyond what its own recoil values imply.
	int DecompMinShots;      // Minimum shots fired in the burst that just ended (1-based, matches
	                         // WeaponMagazined::m_iShotNum / the anm_shots_second/_third/_fourth tiers)
	                         // before on_weapon_shot_stop() fires the decompensation kick at all. Default
	                         // 4 -- the shooter hasn't "fought" any sustained climb worth correcting after
	                         // just 1-3 shots, and 4 lines up with the anm_shots_fourth plateau tier where
	                         // the shot-progression animation itself stops changing.

	// Per-weapon overrides (22/09) for the 6 global "Decompensation" MCM sliders in the Bodycam Weapon
	// Recoil tab (insurgency_decomp_impulse/_vertical_scale/_forward_scale/_pitch_scale/_horizontal_scale/
	// _ads_scale in .ltx) -- each defaults to -1, meaning "not set, inherit the global slider value".
	// Set any subset of these on a weapon to shape ITS decompensation independently of every other
	// InsurgencyRecoil weapon, instead of the one shared global feel. See Bodycam::RecoilDecompOverride
	// (bodycam_simulation.h) for where these actually get resolved/applied.
	float DecompImpulse;
	float DecompVerticalScale;
	float DecompForwardScale;
	float DecompPitchScale;
	float DecompHorizontalScale;
	float DecompAdsScale;

	CameraRecoil():
		MaxAngleVert(EPS),
		RelaxSpeed(EPS_L),
		RelaxSpeed_AI(EPS_L),
		Dispersion(EPS),
		DispersionInc(0.0f),
		DispersionFrac(1.0f),
		MaxAngleHorz(EPS),
		StepAngleHorz(0.0f),
		ReturnMode(false),
		StopReturn(false),
		InsurgencyRecoil(false),
		YawRho(0.0f),
		LeanCoupling(0.0f),
		RiseTimeMs(0.0f),
		MuzzlePivot(0.0f),
		YawCenterPull(0.0f),
		DecompScale(1.0f),
		DecompMinShots(4),
		DecompImpulse(-1.0f),
		DecompVerticalScale(-1.0f),
		DecompForwardScale(-1.0f),
		DecompPitchScale(-1.0f),
		DecompHorizontalScale(-1.0f),
		DecompAdsScale(-1.0f)
	{
	};

	CameraRecoil(const CameraRecoil& clone) { Clone(clone); }

	IC void Clone(const CameraRecoil& clone)
	{
		// *this = clone;
		RelaxSpeed = clone.RelaxSpeed;
		RelaxSpeed_AI = clone.RelaxSpeed_AI;
		Dispersion = clone.Dispersion;
		DispersionInc = clone.DispersionInc;
		DispersionFrac = clone.DispersionFrac;
		MaxAngleVert = clone.MaxAngleVert;
		MaxAngleHorz = clone.MaxAngleHorz;
		StepAngleHorz = clone.StepAngleHorz;

		ReturnMode = clone.ReturnMode;
		StopReturn = clone.StopReturn;

		InsurgencyRecoil = clone.InsurgencyRecoil;
		YawRho = clone.YawRho;
		LeanCoupling = clone.LeanCoupling;
		RiseTimeMs = clone.RiseTimeMs;
		MuzzlePivot = clone.MuzzlePivot;
		YawCenterPull = clone.YawCenterPull;
		DecompScale = clone.DecompScale;
		DecompMinShots = clone.DecompMinShots;
		DecompImpulse = clone.DecompImpulse;
		DecompVerticalScale = clone.DecompVerticalScale;
		DecompForwardScale = clone.DecompForwardScale;
		DecompPitchScale = clone.DecompPitchScale;
		DecompHorizontalScale = clone.DecompHorizontalScale;
		DecompAdsScale = clone.DecompAdsScale;

		VERIFY(!fis_zero(RelaxSpeed));
		VERIFY(!fis_zero(RelaxSpeed_AI));
		VERIFY(!fis_zero(MaxAngleVert));
		VERIFY(!fis_zero(MaxAngleHorz));
	}
}; //struct CameraRecoil

#endif // CAMERA_RECOIL_H_INCLUDED
