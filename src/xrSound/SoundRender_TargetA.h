#ifndef SoundRender_TargetAH
#define SoundRender_TargetAH
#pragma once

#include "soundrender_Target.h"
#include "soundrender_CoreA.h"

class CSoundRender_TargetA : public CSoundRender_Target
{
	typedef CSoundRender_Target inherited;

public:
	// OpenAL
	ALuint pSource;
	ALuint pBuffers[sdef_target_count];
	float cache_gain;
	float cache_pitch;
	ALuint Slot;

	// Phase 1/2 occlusion rework (29/09, "snd_occlusion_mode 1"). Created once per pooled target (if
	// EFX is supported) regardless of mode, so mode can be toggled live without recreating targets;
	// only ACTUALLY bound to the source (render()) when mode 1 is active for that voice. cache_hf avoids
	// redundant alFilterf calls, same pattern as cache_gain/cache_pitch above.
	ALuint m_direct_filter;
	ALuint m_send_filter;
	float cache_hf;
	float cache_gain_direct; // 30/09: broadband AL_LOWPASS_GAIN cache, direct filter
	float cache_gain_wet;    // 30/09: broadband AL_LOWPASS_GAIN cache, send filter (independent of direct)

	ALuint buf_block;
private:
	void fill_block(ALuint BufferID);
public:
	CSoundRender_TargetA();
	virtual ~CSoundRender_TargetA();

	void SetSlot(ALuint NewSlot);
	virtual BOOL _initialize();
	virtual void _destroy();
	virtual void _restart();

	virtual void start(CSoundRender_Emitter* E);
	virtual void render();
	virtual void rewind();
	virtual void stop();
	virtual void update();
	virtual void fill_parameters();
	void source_changed();
};
#endif
