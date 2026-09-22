#pragma once

#include <cstdint>

namespace Bodycam
{
constexpr float kDefaultStalker2MovementResponse = 5.f;

struct SVec3
{
	float x = 0.f;
	float y = 0.f;
	float z = 0.f;

	void Set(float nx, float ny, float nz);
	void Add(const SVec3& value);
	void Add(float nx, float ny, float nz);
	void Sub(const SVec3& value);
	void Mul(float value);
	float Magnitude() const;
	void NormalizeSafe();
};

float ClampSprintBridgeHandoffSpeed(float value);

enum ESimMoveFlags : std::uint32_t
{
	smfForward = 1u << 0,
	smfBack = 1u << 1,
	smfLeft = 1u << 2,
	smfRight = 1u << 3,
	smfCrouch = 1u << 4,
	smfSprint = 1u << 5,
	smfFall = 1u << 6,
	smfJump = 1u << 7,
	smfLanding = 1u << 8,
	smfAnyMove = smfForward | smfBack | smfLeft | smfRight,
};

struct SimulationFeatureSettings
{
	bool camera_enable = true;
	bool vm_enable = false;
	bool lower_enable = true;
	bool bodycam_arm_enable = false;
	bool stalker2_arm_enable = true;
	bool sprint_transition_enable = true;
	bool fire_impulse_enable = true;
	bool impulse_debug = false;
	bool lower_disable_in_combat = true;
	float layer_vm_weight = 1.f;
	float layer_lower_weight = 1.f;
	float layer_arm_weight = 1.f;
};

struct AuthoredMotionMetrics
{
	float lead_rotation = 0.f;
	float lead_translation = 0.f;
	float wrist_rotation = 0.f;
	float forearm_rotation = 0.f;
	float upperarm_rotation = 0.f;
};

struct AuthoredMotionGains
{
	float controller = 0.f;
	float wrist = 0.f;
	float arm = 0.f;
};

struct SimulationCameraModeSettings
{
	float inner_gain = 0.f;
	float spring_freq = 8.f;
	float spring_damping = 0.85f;
	float deadzone_yaw = 1.5f;
	float deadzone_pitch = 1.f;
	float softzone_yaw = 4.f;
	float softzone_pitch = 3.f;
	float max_yaw = 19.f;
	float max_pitch = 19.f;
	float roll = 2.5f;
	float pos = 0.025f;
};

struct SimulationCameraSettings
{
	SimulationCameraModeSettings hip;
	SimulationCameraModeSettings ads = { 0.f, 14.f, 1.f, 0.8f, 0.55f, 1.8f, 1.2f, 3.f, 2.f, 0.6f, 0.006f };
	float move_roll = 3.f;
	float move_pos = 0.020f;
};

struct SimulationViewmodelSettings
{
	float follow_speed = 9.f;
	float damping = 0.78f;
	float mouse_pos = 0.030f;
	float mouse_rot = 5.0f;
	float max_pos = 0.035f;
	float max_rot = 5.f;
	float ads_mouse_mult = 0.18f;
	float ads_impulse_mult = 0.18f;
	float mouse_filter = 18.f;
	float move_filter = 8.f;
	float ads_anchor = 0.65f;
	float ads_anchor_pos = 0.010f;
	float ads_anchor_rot = 1.2f;

	// Insurgency-style recoil follow: unlike mouse-throw (reacts to look *velocity*, fades once the
	// rate settles) and the per-shot fire impulse (decays back to zero regardless of where the real
	// recoil currently sits), this channel springs toward the *actual accumulated* camera recoil
	// (SimulationInput::recoil_pitch/yaw) -- so the viewmodel keeps climbing with the camera during a
	// sustained burst and eases back down together with it once the camera relaxes. Zero input (any
	// non-InsurgencyRecoil weapon) means zero effect regardless of these scales. See
	// ActorCameras.cpp::cam_BodycamVisualUpdate for where recoil_pitch/yaw come from.
	// Vertical/horizontal split (15/09, replacing a single shared follow_speed/damping): lets the
	// horizontal contribution visibly lag behind the camera on direction reversals (a slower horizontal
	// follow_speed means the viewmodel hasn't finished catching up to the old direction when the camera
	// reverses) without touching the already-validated vertical climb-follow feel.
	float recoil_follow_speed_vert = 6.f; // looser than the mouse-throw spring (follow_speed) -- more lag/lead
	float recoil_follow_damping_vert = 0.75f;
	float recoil_follow_speed_horz = 6.f;
	float recoil_follow_damping_horz = 0.75f;
	// Vertical (pitch/climb) and horizontal (yaw/roll) scales are separate: weapons with a strong,
	// AR(1)-correlated horizontal step (insurgency_yaw_rho) can make the horizontal contribution feel
	// erratic/excessive at a shared scale, since it sometimes holds direction for several shots and
	// sometimes flips -- keeping them independent lets you keep a strong vertical climb-follow while
	// dialing the horizontal contribution down (or off) without touching the other.
	float recoil_pos_scale_vert = 0.0005f;
	float recoil_pos_scale_horz = 0.0005f;
	float recoil_rot_scale_vert = 0.2f;
	float recoil_rot_scale_horz = 0.2f;
	// Muzzle-pivot geometry (meters, in the same x=lateral/y=vertical/z=forward space as viewmodel_pos):
	// where the "anchor" (roughly the grip/wrist) sits relative to the viewmodel's own origin. Only the
	// vertical (pitch) component of the recoil rotation drives this -- horizontal is untouched by
	// design, so it can't amplify horizontal drift the way a shared rotation scale did. Global geometry,
	// shared by every insurgency_muzzle_pivot weapon; each weapon's CameraRecoil::MuzzlePivot (0-1+,
	// .ltx `insurgency_muzzle_pivot`) scales how much of it applies -- 0 (default) reproduces today's
	// rigid whole-viewmodel rotation exactly. Sign of pivot_y/pivot_z not verified in-game yet; flip if
	// the muzzle appears to dip instead of rise.
	float recoil_pivot_y = -0.05f;
	float recoil_pivot_z = 0.12f;
	// ADS blend target for this channel specifically (Lerp(1, recoil_ads_mult, ads_blend), independent
	// of ads_impulse_mult which still governs the fire impulse/mouse-throw channels). Default 0: no
	// viewmodel/camera decoupling at all once fully aimed, matching Insurgency's ADS behaviour and
	// avoiding any PIP scope tube/parallax misalignment. Raise only if you deliberately want some ADS
	// sway back.
	float recoil_ads_mult = 0.f;
};

// Idle/aim weapon sway: a continuous, viewmodel-only wobble (never touches the real camera/aim, unlike
// the Lua weapon_sway.script/shaking_hands() systems it's meant to eventually replace, which move the
// camera itself via level.add_cam_effector). Two additive layers per weapon: a periodic component (two
// sine harmonics so it doesn't look like a metronome) plus an independent smoothed-random-walk noise
// layer (same exponential-approach technique as insurgency_yaw_center_pull) for organic, non-repeating
// drift -- the "noise" knob a weapon's .ltx can raise for a genuinely unique, less mechanical pattern.
// Per-weapon pattern (amplitude/frequencies/mix/noise) lives in SimulationInput (bodycam_sway_* .ltx
// keys); this struct holds the global, MCM-tunable feel multipliers shared by every weapon.
struct SimulationSwaySettings
{
	bool enable = true;
	float amplitude_pos_mult = 1.f;
	float amplitude_rot_mult = 1.f;
	// Lerp(1, ads_mult, ads_blend) -- sway calms down once aimed (steadier hold via sights), same blend
	// pattern as SimulationViewmodelSettings::recoil_ads_mult.
	float ads_mult = 0.35f;
	// Global time-base multiplier for the whole sway channel (periodic phase advance + noise-walk rate).
	// Scales speed only, not amplitude -- lets the pattern read as a slow, subtle drift instead of a fast
	// wobble without having to retune every per-weapon amplitude/frequency key. 1 = unscaled, <1 = slower.
	float speed_scale = 1.f;

	// Hold-breath (18/09 follow-up): replaces TheTazDJ's weapon_sway.script hold-breath (same shape --
	// depth while held, a max hold duration, a post-release "out of breath" penalty -- but viewmodel-only,
	// no real camera effector). hold_breath_mult is the depth while actively (and successfully) holding;
	// max_time caps how long that benefit lasts before the character is treated as having let go even if
	// the key is still down; restore_rate is how fast the held-for timer drains once released, in
	// seconds-of-debt per second; release_penalty_mult (>1) is the shakier-than-baseline multiplier applied
	// while held-for is still above threshold after releasing (or after hitting max_time).
	float hold_breath_mult = 0.15f;
	float hold_breath_max_time = 11.f;
	float hold_breath_restore_rate = 0.5f;
	float hold_breath_release_penalty_mult = 1.35f;
	float hold_breath_threshold = 2.f;

	// Arm injury (18/09 follow-up): replaces ZZZ Patch's shaking_hands() NEW_LIMB_PENALTIES_FEATURE (same
	// intent -- hurt arms shake more -- but viewmodel-only). Lerp(1, injury_mult, severity), severity is a
	// 0..1 fraction pushed from a script polling the Body Health System's health.leftarm/rightarm globals.
	float injury_mult = 2.f;

	// Reserved for the next pass: weapon ergonomics, stamina/fatigue, post-sprint recovery window. Present
	// now so this MCM tab doesn't need a second pass later, but currently inert -- nothing pushes real
	// values into SimulationInput's (not yet added) dynamic-state fields yet. Do not remove pending that.
	float weight_mult = 1.f;
	float ergonomics_mult = 1.f;
	float fatigue_mult = 1.f;
	float sprint_recovery_mult = 1.f;
	float sprint_recovery_duration = 3.f;
};

struct SimulationImpulseSettings
{
	float decay = 8.f;
	float sprint_impulse = 1.f;
	float sprint_start_impulse = 5.f;
	float sprint_stop_impulse = 5.f;
	float sprint_ads_mult = 0.2f;
	float sprint_camera_impulse = 5.f;
	float sprint_fov_impulse = 5.f;
	float sprint_fov_speed = 0.5f;
	float sprint_impulse_speed = 0.2f;
	float ads_impulse = 1.f;
	float land_impulse = 1.f;
	float flick_impulse = 0.f;
	float fire_impulse = 5.f;
	float ads_fire_impulse = 1.35f;

	// Recoil decompensation (21/09): a real shooter actively fighting sustained recoil has their own
	// compensating (downward) muscular force suddenly left with nothing to counter the instant firing
	// actually stops -- the arms briefly dip down and forward past neutral before settling, most visible
	// on hard-kicking rifles/battle-rifle calibres, still present but subtle on something like the UZI.
	// Viewmodel-only (this whole impulse system never touches the real camera/aim). Scaled by the LAST
	// shot's own kick magnitude (CWeaponShotEffector::GetLastShotImpulse, same value the per-shot fire
	// impulse above already uses) rather than the burst's accumulated total, which saturates against
	// cam_max_angle regardless of burst length -- a 10-shot and a 50-shot sustained burst that both
	// reached steady-state recoil should decompensate about the same amount, not wildly differently.
	float recoil_decomp_impulse = 8.f; // overall intensity, same role as fire_impulse above
	float recoil_decomp_vertical_scale = 1.f; // downward position dip
	float recoil_decomp_forward_scale = 1.f; // forward (muzzle-away) position push
	float recoil_decomp_pitch_scale = 1.f; // anti-rise: reverse-pitch rotation, opposite sign from the climb
	float recoil_decomp_horizontal_scale = 0.3f; // sideways position + roll -- secondary/optional, modest default
	float recoil_decomp_ads_scale = 0.5f; // multiplier while aiming (tighter grip = less dip)

	float impulse_pos_cap = 0.08f;
	float impulse_rot_cap = 8.f;
};

// Per-weapon override for the recoil_decomp_* fields above (insurgency_decomp_impulse etc. in .ltx,
// see Weapon.cpp/CameraRecoil.h) -- each field defaults to kDecompUseGlobal (-1), meaning "this weapon
// doesn't override it, use the global Bodycam Weapon Recoil MCM slider instead". Lets each weapon be
// tuned individually (a snappy SMG vs. a heavy MG shouldn't decompensate the same way) without having
// to touch the shared global sliders, which stay as sane defaults for every other InsurgencyRecoil
// weapon that doesn't set any of these keys.
constexpr float kDecompUseGlobal = -1.f;
struct RecoilDecompOverride
{
	float impulse = kDecompUseGlobal;
	float vertical_scale = kDecompUseGlobal;
	float forward_scale = kDecompUseGlobal;
	float pitch_scale = kDecompUseGlobal;
	float horizontal_scale = kDecompUseGlobal;
	float ads_scale = kDecompUseGlobal;
};

struct SimulationSprintSettings
{
	float strength = 1.5f;
	float smoothness = 1.f;
	float enter_time = 0.32f;
	float exit_time = 0.48f;
	float camera_pitch = -0.25f;
	float camera_roll = 0.25f;
	float camera_pos = 0.004f;
	float bridge_pitch = -8.f;
	float bridge_yaw = -8.f;
	float bridge_roll = -7.7f;
	float bridge_pos = 0.050f;
	float bridge_handoff_speed = 0.90f;
	float accent = 2.f;
};

struct SimulationLoweringSettings
{
	float pitch = 0.f;
	float yaw = 0.f;
	float roll = 0.f;
	float x = 0.f;
	float y = -0.08f;
	float z = 0.1f;
	float holster_offset = 0.015f;
	float slow_walk = 0.5f;
	float walk = 1.f;
	float move = 1.f;
	float fire_timeout = 0.03f;
	float aim_timeout = 0.005f;
	float speed = 0.05f;
	float return_speed = 0.4f;
	float combat_timeout = 2.f;
};

struct SimulationArmSettings
{
	float strength = 3.f;
	float response = 30.f;
	float ads_scale = 1.f;
	float mouse_pitch = 30.f;
	float mouse_yaw = 30.f;
	float mouse_roll = 45.f;
	float secondary_roll = 20.f;
	float hand_scale = 0.f;
	float upperarm_scale = 0.05f;
	float forearm_scale = 0.1f;
	float twist_scale = 2.f;
};

struct SimulationStalker2ArmSettings
{
	float strength = 1.5f;
	float response = 20.f;
	float arm_follow_response = 0.1f;
	float arm_follow_scale = 0.f;
	float ads_scale = 0.2f;
	float mouse_strength = 1.f;
	float mouse_sensitivity = 1.5f;
	float mouse_max_yaw = 0.f;
	float mouse_max_pitch = 18.f;
	float mouse_max_roll = 15.5f;
	float movement_strength = 1.f;
	float movement_response = kDefaultStalker2MovementResponse;
	float slow_walk_scale = 0.5f;
	float mouse_pitch = 18.f;
	float mouse_yaw = 0.f;
	float mouse_roll = 28.f;
	float wrist_scale = 1.f;
};

struct SimulationSettings
{
	SimulationFeatureSettings features;
	SimulationCameraSettings camera;
	SimulationViewmodelSettings viewmodel;
	SimulationImpulseSettings impulse;
	SimulationSprintSettings sprint;
	SimulationLoweringSettings lowering;
	SimulationArmSettings bodycam_arm;
	SimulationStalker2ArmSettings stalker2_arm;
	SimulationSwaySettings sway;
};

struct SimulationInput
{
	float target_yaw = 0.f;
	float target_pitch = 0.f;
	float dt = 0.f;
	std::uint32_t move_flags = 0;
	bool ads = false;
	float ads_blend = 0.f;
	bool weapon_lowered = false;
	bool combat = false;
	bool accelerated = false;
	bool firearm_equipped = true;
	float actor_speed_fraction = 0.f;
	bool visual_aim_available = false;
	float visual_aim_yaw = 0.f;
	float visual_aim_pitch = 0.f;

	// Real, accumulated camera recoil this frame (CWeaponShotEffector::GetOutputVert/Horz -- same raw
	// units as the weapon's cam_max_angle etc, already past rise-time easing). Zero for any weapon that
	// isn't opted into InsurgencyRecoil, which keeps the viewmodel-follow channel a no-op for them.
	float recoil_pitch = 0.f;
	float recoil_yaw = 0.f;
	float muzzle_pivot = 0.f; // CameraRecoil::MuzzlePivot of the active weapon; 0 for non-InsurgencyRecoil weapons
	float yaw_center_pull = 0.f; // CameraRecoil::YawCenterPull; also applied to this channel's own spring
	                             // state (recoil_pos.x, recoil_rot.y/.z), not just m_angle_horz upstream --
	                             // the spring's own lag otherwise keeps visibly leaning one way for a while
	                             // after the upstream signal has already been pulled back toward center.

	// Per-weapon idle/aim sway pattern (bodycam_sway_* / zoom_bodycam_sway_* .ltx keys on the active
	// weapon). sway_enabled false (the default, absent keys) means zero amplitude -> the whole channel
	// is a no-op, same backward-compatibility guarantee as every other Insurgency/Bodycam addition.
	bool sway_enabled = false;
	float sway_amplitude_pos = 0.f; // meters, hip baseline (before SimulationSwaySettings' global mults)
	float sway_amplitude_rot = 0.f; // degree-like, same convention as the rest of this file
	float sway_freq_primary = 0.4f; // Hz-ish; slow "breathing" component
	float sway_freq_secondary = 1.1f; // Hz-ish; faster "micro-tremor" component
	float sway_mix_secondary = 0.35f; // 0 = pure primary sine, 1 = pure secondary
	float sway_noise_amplitude = 0.f; // 0 = purely periodic (metronomic); >0 blends in organic drift
	float sway_noise_rate = 0.6f; // how fast the noise layer's target wanders, in 1/s (same units/technique as yaw_center_pull)

	// Is the player currently holding the hold-breath key (bodycam.set_hold_breath, pushed from a script
	// that polls its own kCUSTOM key bind). Independent of any per-weapon key -- scales sway amplitude
	// down (via SimulationSwaySettings::hold_breath_mult) rather than freezing sway_pos/sway_rot outright,
	// so an already-off-center reticle eases back toward zero instead of getting stuck offset.
	bool hold_breath_active = false;

	// 0..1 arm injury severity (bodycam.set_arm_injury, pushed from a script polling the Body Health
	// System's health.leftarm/health.rightarm globals -- there's no change-event for those, so it's a
	// poll). 0 = both arms healthy (no-op), 1 = an arm fully broken (full SimulationSwaySettings::injury_mult).
	float arm_injury_severity = 0.f;
};

struct SimulationCameraState
{
	bool initialized = false;
	float yaw = 0.f;
	float pitch = 0.f;
	float roll = 0.f;
	SVec3 pos;
	SVec3 impulse_pos;
	float impulse_roll = 0.f;
	float impulse_fov = 0.f;
	float impulse_fov_target = 0.f;
};

struct SimulationViewmodelState
{
	SVec3 pos;
	SVec3 rot;
	SVec3 mouse_speed;
	SVec3 prev_mouse_speed;
	SVec3 mouse_accel;
	float mouse_aim_yaw = 0.f;
	float mouse_aim_pitch = 0.f;
	bool mouse_aim_initialized = false;
	SVec3 move_intent;
	SVec3 impulse_pos;
	SVec3 impulse_rot;
	SVec3 fire_impulse_pos;
	SVec3 fire_impulse_rot;
	SVec3 fire_pos;
	SVec3 fire_rot;
	// Recoil decompensation's own channel (21/09), kept separate from impulse_pos/rot on purpose: it must
	// NOT be scaled by the shared viewmodel.ads_impulse_mult downstream (same reasoning as recoil_pos/rot
	// using their own recoil_ads_mult instead of that shared multiplier) -- recoil_decomp_ads_scale is
	// already applied once, at the moment the impulse is added (see AddRecoilDecompImpulse), so this is
	// the ONLY ADS attenuation it gets. Without this separation it silently stacked with ads_impulse_mult
	// (default 0.18), making decompensation nearly invisible in ADS regardless of its own slider.
	SVec3 decomp_pos;
	SVec3 decomp_rot;
	SVec3 recoil_pos;
	SVec3 recoil_rot;
	float airborne_time = 0.f;

	// Idle/aim sway state (see SimulationSwaySettings). sway_phase drives the periodic component;
	// sway_noise is the smoothed-random-walk state; sway_rng is a small self-contained PRNG seed (this
	// file avoids xrCore under BODYCAM_STANDALONE, so it can't reuse CRandom). sway_pos/rot are this
	// channel's current contribution, kept for the debug-log breakdown the same way recoil_pos/rot are.
	float sway_phase = 0.f;
	SVec3 sway_noise;
	std::uint32_t sway_rng = 0;
	SVec3 sway_pos;
	SVec3 sway_rot;
	// Smoothed current hold-breath amplitude multiplier (not a 0..1 blend -- directly the multiplier being
	// eased toward, since the target itself switches between 1 / hold_breath_mult / release_penalty_mult).
	// Starts at 1 (no effect) rather than 0.
	float sway_hold_breath_blend = 1.f;
	// Seconds of "breath held" credit: rises while hold_breath_active, drains at hold_breath_restore_rate
	// otherwise. Drives both the max-hold-time cutoff and the post-release penalty window.
	float hold_breath_held_for = 0.f;
	// Smoothed 0..1 arm injury severity (eases discrete health.leftarm/rightarm steps instead of snapping).
	float sway_injury_blend = 0.f;
};

struct SimulationLoweringState
{
	float amount = 0.f;
	float target = 0.f;
	float holster = 0.f;
	float fire_recovery = 0.f;
	float ads_recovery = 0.f;
	float combat_timer = 0.f;
	SVec3 pos;
	SVec3 rot;
};

struct SimulationSprintImpulseState
{
	SVec3 camera_pos;
	float camera_roll = 0.f;
};

struct SimulationSprintState
{
	float amount = 0.f;
	float target = 0.f;
	float viewmodel_amount = 0.f;
	float settle = 0.f;
	float phase = 0.f;
	float prev_amount = 0.f;
};

struct SimulationArmState
{
	SVec3 clavicle;
	SVec3 upperarm;
	SVec3 forearm;
	SVec3 twist;
	SVec3 hand;
	SVec3 left_clavicle;
	SVec3 left_upperarm;
	SVec3 left_forearm;
	SVec3 left_twist;
	SVec3 left_hand;
	SVec3 controller;
	SVec3 prev_controller;
	SVec3 settle;
	SVec3 clavicle_vel;
	SVec3 upperarm_vel;
	SVec3 forearm_vel;
	SVec3 twist_vel;
	SVec3 hand_vel;
	SVec3 left_clavicle_vel;
	SVec3 left_upperarm_vel;
	SVec3 left_forearm_vel;
	SVec3 left_twist_vel;
	SVec3 left_hand_vel;
	float weight = 0.f;
	float motion_weight = 0.f;
};

struct SimulationStalker2ArmState
{
	SVec3 wrist_rot;
	SVec3 wrist_rot_vel;
	SVec3 arm_follow_rot;
	SVec3 arm_follow_rot_vel;
	float weight = 0.f;
	float movement_weight = 0.f;
};

struct AdsState
{
	bool active = false;
	float blend = 0.f;
};

struct SimulationState
{
	SimulationCameraState camera;
	SimulationViewmodelState viewmodel;
	SimulationLoweringState lowering;
	SimulationSprintImpulseState sprint_impulse;
	SimulationSprintState sprint;
	SimulationArmState arm;
	SimulationStalker2ArmState stalker2_arm;
	float ads_blend = 0.f;
	std::uint32_t prev_move_flags = 0;
	bool prev_ads = false;
};

struct SimulationOutput
{
	float yaw = 0.f;
	float pitch = 0.f;
	float roll = 0.f;
	SVec3 camera_pos;
	float fov_offset = 0.f;
	float lower_amount = 0.f;
	bool viewmodel_active = false;
	SVec3 viewmodel_pos;
	SVec3 viewmodel_rot;
	bool arm_active = false;
	SVec3 arm_clavicle;
	SVec3 arm_upperarm;
	SVec3 arm_forearm;
	SVec3 arm_twist;
	SVec3 arm_hand;
	SVec3 arm_left_clavicle;
	SVec3 arm_left_upperarm;
	SVec3 arm_left_forearm;
	SVec3 arm_left_twist;
	SVec3 arm_left_hand;
	bool stalker2_arm_active = false;
	SVec3 stalker2_wrist_rot;
	SVec3 stalker2_arm_follow_rot;
	float stalker2_movement_weight = 0.f;
	float stalker2_movement_response = kDefaultStalker2MovementResponse;
	bool impulse_pos_clamped = false;
	bool impulse_rot_clamped = false;
};

void ResetSimulation(SimulationState& state, float yaw, float pitch, std::uint32_t move_flags, float ads_blend);
void RebaseSimulationLook(SimulationState& state, float yaw_delta, float pitch_delta);
AdsState ResolveAdsState(bool weapon_zoomed, float weapon_blend);

void UpdateSimulation(const SimulationSettings& settings, SimulationState& state, const SimulationInput& input, SimulationOutput& output);
void AddFireImpulse(const SimulationSettings& settings, SimulationState& state, float power, bool ads);
void AddRecoilDecompImpulse(const SimulationSettings& settings, SimulationState& state, float power, bool ads,
	const RecoilDecompOverride& overrides);
bool AddNamedImpulse(const SimulationSettings& settings, SimulationState& state, const char* kind, float power, bool ads);
float ArmCorrectionAngle(float dot, float cross_magnitude);
SVec3 SolveArmMidpoint(const SVec3& start, const SVec3& end, const SVec3& current_mid, const SVec3& desired_mid);
AuthoredMotionGains CalculateAuthoredMotionGains(const AuthoredMotionMetrics& metrics);
SVec3 CalculateAuthoredWalkRotation(float phase, float weight, const AuthoredMotionGains& gains);
SVec3 CalculateAuthoredWalkTranslation(float phase, float weight, const AuthoredMotionGains& gains);
float CalculateAuthoredArmFollow(float arm_gain);
SVec3 CalculateStalker2MouseControllerRotation(float yaw_throw, float yaw_scale, float roll_scale, float weight);
SVec3 CalculateStalker2VerticalArmFollow(float pitch_throw, float pitch_scale, float weight);
float CalculateMouseThrow(float angular_speed, float full_scale_speed, float sensitivity);
float SoftLimitMouseResponse(float value, float limit);
SVec3 ClampStalker2MouseRotation(const SVec3& rotation, float max_yaw, float max_pitch, float max_roll);
float CalculateStalker2MovementAmount(float move_intent, float speed_fraction, bool accelerated, float slow_walk_scale);
float DegToRad(float value);
float RadToDeg(float value);
} // namespace Bodycam
