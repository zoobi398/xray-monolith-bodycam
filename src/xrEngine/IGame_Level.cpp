#include "stdafx.h"
#include "igame_level.h"
#include "igame_persistent.h"

#include "x_ray.h"
#include "std_classes.h"
#include "customHUD.h"
#include "render.h"
#include "gamefont.h"
#include "xrLevel.h"
#include "CameraManager.h"
#include "xr_object.h"
#include "feel_sound.h"

#include "../xrCore/profiler.h"
#include "GameMtlLib.h"

//#include "securom_api.h"

ENGINE_API IGame_Level* g_pGameLevel = NULL;
extern BOOL g_bLoaded;

IGame_Level::IGame_Level()
{
	PROF_EVENT("IGame_Level::IGame_Level");
	m_pCameras = xr_new<CCameraManager>(true);
	g_pGameLevel = this;
	pLevel = NULL;
	bReady = false;
	pCurrentEntity = NULL;
	pCurrentViewEntity = NULL;
	Device.DumpResourcesMemoryUsage();
}

//#include "resourcemanager.h"

IGame_Level::~IGame_Level()
{
	Device.secondary_tasks.wait();

	if (Core.ParamsData.test(ECoreParams::nes_texture_storing))
		Device.m_pRender->ResourcesStoreNecessaryTextures();
	xr_delete(pLevel);

	// Render-level unload
	Render->level_Unload();
	xr_delete(m_pCameras);
	// Unregister
	Device.seqParallel.clear_not_free();
	Device.seqRender.Remove(this);
	Device.seqFrame.Remove(this);
	CCameraManager::ResetPP();
	///////////////////////////////////////////
	Sound->set_geometry_occ(NULL);
	Sound->set_handler(NULL);
	Sound->set_handler_raw(NULL);
	Device.DumpResourcesMemoryUsage();

	u32 m_base = 0, c_base = 0, m_lmaps = 0, c_lmaps = 0;
	if (Device.m_pRender)
		Device.m_pRender->ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);

	Msg("* [ D3D ]: textures[%d K]", (m_base + m_lmaps) / 1024);
}

void IGame_Level::net_Stop()
{
    for (int i = 0; i < 6; i++)
    {
        Objects.Update(false);
        Objects.ProcessDestroyQueue();
    }
		
	// Destroy all objects
	Objects.Unload();
	IR_Release();

	bReady = false;
}

//-------------------------------------------------------------------------------------------
//extern CStatTimer tscreate;
void __stdcall _sound_event(ref_sound_data_ptr S, float range)
{
	if (g_pGameLevel && S && S->feedback) g_pGameLevel->SoundEvent_Register(S, range);
}

void __stdcall _sound_event_raw(CObject* who, int type, const Fvector& pos, float max_ai_dist, float volume)
{
	if (g_pGameLevel) g_pGameLevel->SoundEvent_RegisterRaw(who, type, pos, max_ai_dist, volume);
}

static void __stdcall build_callback(Fvector* V, int Vcnt, CDB::TRI* T, int Tcnt, void* params)
{
	g_pGameLevel->Load_GameSpecific_CFORM(T, Tcnt);
}

// Phase 1/2 occlusion rework (29/09), "snd_occlusion_mode 1". Builds the flat, GameMtl-ID-indexed
// material table xrSound needs (xrSound can't depend on GMLib itself, see Sound.h) and hands it over.
// Runs once per level load, right after Sound->set_geometry_occ() below -- negligible cost (a few
// hundred materials at most), never in the per-frame hot path.
static void BuildAndPushOcclusionMaterials()
{
	SSoundOcclusionMaterial default_mat;
	default_mat.loss_db = 10.f;
	default_mat.loss_db_per_m = 1.f;
	default_mat.hf_loss_db = 12.f;
	default_mat.ignore = false;

	float max_loss_db = 28.f, max_hf_loss_db = 36.f, max_thickness_m = 12.f;
	float max_loss_db_indoor = -1.f, max_hf_loss_db_indoor = -1.f; // -1 = not given -> same as outdoor

	string_path fname;
	CInifile* ini = FS.exist(fname, "$game_config$", "sound_occlusion.ltx") ? xr_new<CInifile>(fname, TRUE) : nullptr;

	if (ini)
	{
		if (ini->section_exist("sound_occlusion_limits"))
		{
			LPCSTR sec = "sound_occlusion_limits";
			if (ini->line_exist(sec, "max_loss_db")) max_loss_db = ini->r_float(sec, "max_loss_db");
			if (ini->line_exist(sec, "max_hf_loss_db")) max_hf_loss_db = ini->r_float(sec, "max_hf_loss_db");
			if (ini->line_exist(sec, "max_thickness_m")) max_thickness_m = ini->r_float(sec, "max_thickness_m");
			if (ini->line_exist(sec, "max_loss_db_indoor")) max_loss_db_indoor = ini->r_float(sec, "max_loss_db_indoor");
			if (ini->line_exist(sec, "max_hf_loss_db_indoor")) max_hf_loss_db_indoor = ini->r_float(sec, "max_hf_loss_db_indoor");
		}
		if (ini->section_exist("default"))
		{
			LPCSTR sec = "default";
			if (ini->line_exist(sec, "loss_db")) default_mat.loss_db = ini->r_float(sec, "loss_db");
			if (ini->line_exist(sec, "loss_db_per_m")) default_mat.loss_db_per_m = ini->r_float(sec, "loss_db_per_m");
			if (ini->line_exist(sec, "hf_loss_db")) default_mat.hf_loss_db = ini->r_float(sec, "hf_loss_db");
			if (ini->line_exist(sec, "ignore")) default_mat.ignore = ini->r_bool(sec, "ignore");
		}
	}

	// Resolves one GameMtl to a material class: first substring match in [sound_occlusion_classes]
	// (checked in the order the ltx declares them), then this material's own legacy
	// sound_occlusion_factor (materials\*.ltx) converted to a loss_db if it's set and nonzero, then
	// [default]/default_mat as the final fallback.
	auto resolve = [&](SGameMtl* mat) -> SSoundOcclusionMaterial
	{
		if (ini && ini->section_exist("sound_occlusion_classes"))
		{
			CInifile::Sect& S = ini->r_section("sound_occlusion_classes");
			for (CInifile::SectCIt I = S.Data.begin(); I != S.Data.end(); ++I)
			{
				if (!strstr(mat->m_Name.c_str(), I->first.c_str()))
					continue;
				LPCSTR class_name = I->second.c_str();
				if (0 == xr_strcmp(class_name, "ignore"))
				{
					SSoundOcclusionMaterial m = default_mat;
					m.ignore = true;
					return m;
				}
				if (ini->section_exist(class_name))
				{
					SSoundOcclusionMaterial m = default_mat;
					if (ini->line_exist(class_name, "loss_db")) m.loss_db = ini->r_float(class_name, "loss_db");
					if (ini->line_exist(class_name, "loss_db_per_m")) m.loss_db_per_m = ini->r_float(class_name, "loss_db_per_m");
					if (ini->line_exist(class_name, "hf_loss_db")) m.hf_loss_db = ini->r_float(class_name, "hf_loss_db");
					if (ini->line_exist(class_name, "ignore")) m.ignore = ini->r_bool(class_name, "ignore");
					return m;
				}
				break; // matched a class name with no matching section -- fall through to the legacy/default path below
			}
		}
		if (mat->fSndOcclusionFactor > 0.f && mat->fSndOcclusionFactor < 1.f)
		{
			SSoundOcclusionMaterial m = default_mat;
			m.loss_db = -20.f * log10f(mat->fSndOcclusionFactor);
			return m;
		}
		return default_mat;
	};

	int max_id = 0;
	for (GameMtlIt it = GMLib.FirstMaterial(); it != GMLib.LastMaterial(); ++it)
		max_id = std::max(max_id, (*it)->GetID());

	xr_vector<SSoundOcclusionMaterial> table(max_id + 1, default_mat);
	for (GameMtlIt it = GMLib.FirstMaterial(); it != GMLib.LastMaterial(); ++it)
	{
		SGameMtl* mat = *it;
		if (mat->GetID() >= 0 && mat->GetID() < (int)table.size())
			table[mat->GetID()] = resolve(mat);
	}

	Sound->set_occlusion_materials(&table.front(), (u32)table.size());
	if (max_loss_db_indoor < 0.f) max_loss_db_indoor = max_loss_db;
	if (max_hf_loss_db_indoor < 0.f) max_hf_loss_db_indoor = max_hf_loss_db;
	Sound->set_occlusion_limits(max_loss_db, max_hf_loss_db, max_thickness_m, max_loss_db_indoor, max_hf_loss_db_indoor);

	xr_delete(ini);
}

xrCriticalSection lloadcs;
bool IGame_Level::Load(u32 dwNum)
{
	PROF_EVENT("IGame_Level::Load");
	xrCriticalSectionGuard guard(&lloadcs);
	if (bReady) return TRUE;
	extern xr_task_group prefetch_task;
	prefetch_task.wait();
	//SECUROM_MARKER_PERFORMANCE_ON(10)

	// Initialize level data
	pApp->Level_Set(dwNum);
	string_path temp;
	if (!FS.exist(temp, "$level$", "level.ltx"))
		Debug.fatal(DEBUG_INFO, "Can't find level configuration file '%s'.", temp);
	pLevel = xr_new<CInifile>(temp);

	// Open
	// g_pGamePersistent->LoadTitle ("st_opening_stream");
	g_pGamePersistent->LoadTitle();
	IReader* LL_Stream = FS.r_open("$level$", "level");
	IReader& fs = *LL_Stream;

	// Header
	hdrLEVEL H;
	fs.r_chunk_safe(fsL_HEADER, &H, sizeof(H));
	R_ASSERT2(XRCL_PRODUCTION_VERSION == H.XRLC_version, "Incompatible level version.");

	// CForms
	// g_pGamePersistent->LoadTitle ("st_loading_cform");
	g_pGamePersistent->LoadTitle();
	ObjectSpace.Load( [](Fvector* V, int Vcnt, CDB::TRI* T, int Tcnt, void* params){g_pGameLevel->Load_GameSpecific_CFORM(T, Tcnt);});
	//Sound->set_geometry_occ ( &Static );
	Sound->set_geometry_occ(ObjectSpace.GetStaticModel());
	BuildAndPushOcclusionMaterials();
	Sound->set_handler(_sound_event);
	Sound->set_handler_raw(_sound_event_raw);

	pApp->LoadSwitch();


	// HUD + Environment
	if (!g_hud)
		g_hud = (CCustomHUD*)NEW_INSTANCE(CLSID_HUDMANAGER);

	// Render-level Load
	Render->level_Load(LL_Stream);
	// tscreate.FrameEnd ();
	// Msg ("* S-CREATE: %f ms, %d times",tscreate.result,tscreate.count);

	// Objects
	g_pGamePersistent->Environment().mods_load();
	R_ASSERT(Load_GameSpecific_Before());
	Objects.Load();
	//. ANDY R_ASSERT (Load_GameSpecific_After ());

	// Done
	FS.r_close(LL_Stream);
	bReady = true;
	if (!g_dedicated_server) IR_Capture();
#ifndef DEDICATED_SERVER
	Device.seqRender.Add(this);
#endif

	Device.seqFrame.Add(this);

	//SECUROM_MARKER_PERFORMANCE_OFF(10)

	return true;
}

#ifndef _EDITOR
#include "../xrCPU_Pipe/ttapi.h"
#endif

int psNET_DedicatedSleep = 5;

void IGame_Level::OnRender()
{
#ifndef DEDICATED_SERVER
	// if (_abs(Device.fTimeDelta)<EPS_S) return;

#ifdef _GPA_ENABLED
    TAL_ID rtID = TAL_MakeID( 1 , Core.dwFrame , 0);
    TAL_CreateID( rtID );
    TAL_BeginNamedVirtualTaskWithID( "GameRenderFrame" , rtID );
    TAL_Parami( "Frame#" , Device.dwFrame );
    TAL_EndVirtualTask();
#endif // _GPA_ENABLED

	// Level render, only when no client output required
	if (!g_dedicated_server)
	{
		{
			PROF_EVENT("IGame_Level::OnRender: Calculate");
			Render->Calculate();
		}
		{
			PROF_EVENT("IGame_Level::OnRender: Render");
			Render->Render();
		}
	}
	else
	{
		Sleep(psNET_DedicatedSleep);
	}

#ifdef _GPA_ENABLED
    TAL_RetireID( rtID );
#endif // _GPA_ENABLED

	// Font
	// pApp->pFontSystem->SetSizeI(0.023f);
	// pApp->pFontSystem->OnRender ();
#endif
}

void IGame_Level::OnFrame()
{
	PROF_EVENT("IGame_Level::OnFrame");
	// Log ("- level:on-frame: ",u32(Device.dwFrame));
	// if (_abs(Device.fTimeDelta)<EPS_S) return;

	// Update all objects
	VERIFY(bReady);
	Objects.Update(false);
	g_hud->OnFrame();

	// Ambience
	if (Sounds_Random.size() && (Device.dwTimeGlobal > Sounds_Random_dwNextTime))
	{
		Sounds_Random_dwNextTime = Device.dwTimeGlobal + ::Random.randI(10000, 20000);
		Fvector pos;
		pos.random_dir().normalize().mul(::Random.randF(30, 100)).add(Device.vCameraPosition);
		int id = ::Random.randI(Sounds_Random.size());
		if (Sounds_Random_Enabled)
		{
			Sounds_Random[id].play_at_pos(0, pos, 0);
			Sounds_Random[id].set_volume(1.f);
			Sounds_Random[id].set_range(10, 200);
		}
	}
}

// ==================================================================================================

void CServerInfo::AddItem(LPCSTR name_, LPCSTR value_, u32 color_)
{
	shared_str s_name(name_);
	AddItem(s_name, value_, color_);
}

void CServerInfo::AddItem(shared_str& name_, LPCSTR value_, u32 color_)
{
	SItem_ServerInfo it;
	// shared_str s_name = CStringTable().translate( name_ );

	// xr_strcpy( it.name, s_name.c_str() );
	xr_strcpy(it.name, name_.c_str());
	xr_strcat(it.name, " = ");
	xr_strcat(it.name, value_);
	it.color = color_;

	if (data.size() < max_item)
	{
		data.push_back(it);
	}
}

void IGame_Level::SetEntity(CObject* O)
{
	if (pCurrentEntity)
		pCurrentEntity->On_LostEntity();

	if (O)
		O->On_SetEntity();

	pCurrentEntity = pCurrentViewEntity = O;
}

void IGame_Level::SetViewEntity(CObject* O)
{
	if (pCurrentViewEntity)
		pCurrentViewEntity->On_LostEntity();

	if (O)
		O->On_SetEntity();

	pCurrentViewEntity = O;
}

void IGame_Level::SoundEvent_Register(ref_sound_data_ptr S, float range)
{
	PROF_EVENT("IGame_Level::SoundEvent_Register");
	if (!g_bLoaded) return;
	if (!S) return;
	if (S->g_object && S->g_object->getDestroy())
	{
		S->g_object = 0;
		return;
	}
	if (0 == S->feedback) return;

	clamp(range, 0.1f, 500.f);

	const CSound_params* p = S->feedback->get_params();
	Fvector snd_position = p->position;
	if (S->feedback->is_2D())
	{
		snd_position.add(Sound->listener_position());
	}

	VERIFY(p && _valid(range));
	range = _min(range, p->max_ai_distance);
	VERIFY(_valid(snd_position));
	VERIFY(_valid(p->max_ai_distance));
	VERIFY(_valid(p->volume));

	// Query objects
	Fvector bb_size = {range, range, range};
	g_SpatialSpace->q_box(snd_ER, 0, STYPE_REACTTOSOUND, snd_position, bb_size);

	// Iterate
	auto it = snd_ER.begin();
	auto end = snd_ER.end();
	for (; it != end; it++)
	{
		Feel::Sound* L = (*it)->dcast_FeelSound();
		if (0 == L) continue;
		CObject* CO = (*it)->dcast_CObject();
		VERIFY(CO);
		if (CO->getDestroy()) continue;

		// Energy and signal
		VERIFY(_valid((*it)->spatial.sphere.P));
		float dist = snd_position.distance_to((*it)->spatial.sphere.P);
		if (dist > p->max_ai_distance) continue;
		VERIFY(_valid(dist));
		VERIFY2(!fis_zero(p->max_ai_distance), S->handle->file_name());
		float Power = (1.f - dist / p->max_ai_distance) * p->volume;
		VERIFY(_valid(Power));
		if (Power > EPS_S)
		{
			float occ = Sound->get_occlusion_to((*it)->spatial.sphere.P, snd_position);
			VERIFY(_valid(occ));
			Power *= occ;
			if (Power > EPS_S)
			{
				_esound_delegate D = {L, S, Power};
				snd_Events.push_back(D);
			}
		}
	}
	snd_ER.clear_not_free();
}

// Cyclic gunfire in the engine (doc 08). Same algorithm as SoundEvent_Register(), fed with the numbers a vanilla
// emitter would have taken from its sound (AI-reaction distance of the OGG comment, volume) instead of a ref_sound.
void IGame_Level::SoundEvent_RegisterRaw(CObject* who, int type, const Fvector& pos, float max_ai_dist, float volume)
{
	PROF_EVENT("IGame_Level::SoundEvent_RegisterRaw");
	if (!g_bLoaded || !who) return;
	if (who->getDestroy()) return;
	if (max_ai_dist < 0.1f) return;

	float range = _min(max_ai_dist, max_ai_dist * volume);
	if (range < 0.1f) return;
	clamp(range, 0.1f, 500.f);

	Fvector bb_size = {range, range, range};
	g_SpatialSpace->q_box(snd_ER, 0, STYPE_REACTTOSOUND, pos, bb_size);

	for (auto it = snd_ER.begin(); it != snd_ER.end(); ++it)
	{
		Feel::Sound* L = (*it)->dcast_FeelSound();
		if (0 == L) continue;
		CObject* CO = (*it)->dcast_CObject();
		if (!CO || CO->getDestroy()) continue;

		float dist = pos.distance_to((*it)->spatial.sphere.P);
		if (dist > max_ai_dist) continue;
		float Power = (1.f - dist / max_ai_dist) * volume;
		if (Power > EPS_S)
		{
			float occ = Sound->get_occlusion_to((*it)->spatial.sphere.P, pos);
			Power *= occ;
			if (Power > EPS_S)
			{
				_esound_raw D = {L, who, type, pos, Power};
				snd_EventsRaw.push_back(D);
			}
		}
	}
	snd_ER.clear_not_free();
}

void IGame_Level::SoundEvent_Dispatch()
{
	PROF_EVENT("IGame_Level::SoundEvent_Dispatch");
	while (!snd_EventsRaw.empty())
	{
		_esound_raw& D = snd_EventsRaw.back();
		if (D.dest && D.who && !D.who->getDestroy())
			D.dest->feel_sound_new(D.who, D.type, CSound_UserDataPtr(), D.pos, D.power);
		snd_EventsRaw.pop_back();
	}
	while (!snd_Events.empty())
	{
		_esound_delegate& D = snd_Events.back();
		VERIFY(D.dest && D.source);
		if (D.source->feedback)
		{
			D.dest->feel_sound_new(
				D.source->g_object,
				D.source->g_type,
				D.source->g_userdata,

				D.source->feedback->is_2D() ? Device.vCameraPosition : D.source->feedback->get_params()->position,
				D.power
			);
		}
		snd_Events.pop_back();
	}
}

// Lain: added
void IGame_Level::SoundEvent_OnDestDestroy(Feel::Sound* obj)
{
	PROF_EVENT("IGame_Level::SoundEvent_OnDestDestroy");
	struct rem_pred
	{
		rem_pred(Feel::Sound* obj) : m_obj(obj)
		{
		}

		bool operator ()(const _esound_delegate& d)
		{
			return d.dest == m_obj;
		}

	private:
		Feel::Sound* m_obj;
	};

	snd_Events.erase(std::remove_if(snd_Events.begin(), snd_Events.end(), rem_pred(obj)),
	                 snd_Events.end());

	// Cyclic gunfire (doc 08): drop the pending hearing events aimed at, or raised by, a destroyed object
	snd_EventsRaw.erase(std::remove_if(snd_EventsRaw.begin(), snd_EventsRaw.end(),
	                                   [obj](const _esound_raw& d) { return d.dest == obj; }),
	                    snd_EventsRaw.end());
}
