#pragma once

// Minimal FMOD Core lifecycle, owned independently of the OpenAL backend
// (CSoundRender_CoreA / SoundRender_TargetA / OpenALDeviceList). Only 2D
// emitters (CSoundRender_EmitterF) are routed through this system - see
// CSoundRender_Core::play()/play_at_pos()/play_no_feedback() in
// SoundRender_Core.cpp for the routing decision.

namespace FMOD
{
	class System;
}

bool FMODCore_Init();
void FMODCore_Update(float dt);
void FMODCore_Shutdown();
bool FMODCore_Available();
FMOD::System* FMODCore_System();
