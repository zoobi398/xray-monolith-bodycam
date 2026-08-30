#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_CoreF.h"
#include "SoundRender_EmitterF.h"

#include <fmod.hpp>
#include <fmod_errors.h>

static FMOD::System* g_fmod_system = nullptr;
static bool g_fmod_available = false;

bool FMODCore_Init()
{
	FMOD_RESULT result = FMOD::System_Create(&g_fmod_system);
	if (result != FMOD_OK)
	{
		Msg("! FMOD: System_Create failed: %s", FMOD_ErrorString(result));
		g_fmod_system = nullptr;
		return false;
	}

	result = g_fmod_system->init(128, FMOD_INIT_NORMAL, nullptr);
	if (result != FMOD_OK)
	{
		Msg("! FMOD: init failed: %s", FMOD_ErrorString(result));
		g_fmod_system->release();
		g_fmod_system = nullptr;
		return false;
	}

	unsigned int version = 0;
	g_fmod_system->getVersion(&version);
	Msg("* FMOD Core initialized (version %08x) - handles 2D audio only, 3D stays on OpenAL", version);

	g_fmod_available = true;
	return true;
}

void FMODCore_Update(float dt)
{
	if (!g_fmod_available) return;
	g_fmod_system->update();
	CSoundRender_EmitterF::update_all(dt);
}

void FMODCore_Shutdown()
{
	if (!g_fmod_available) return;
	CSoundRender_EmitterF::destroy_all();
	g_fmod_system->close();
	g_fmod_system->release();
	g_fmod_system = nullptr;
	g_fmod_available = false;
}

bool FMODCore_Available()
{
	return g_fmod_available;
}

FMOD::System* FMODCore_System()
{
	return g_fmod_system;
}
