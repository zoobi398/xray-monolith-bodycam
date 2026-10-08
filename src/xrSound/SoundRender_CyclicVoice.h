#pragma once
// Cyclic gunfire in the engine -- native voice for the actor's custom_loop_sound weapons.
// Design and rationale: docs 08_ENGINE_CHANGES_CYCLIC_GUNFIRE_IN_ENGINE.md (00_SUIVI_CHANGEMENTS_ENGINE).
//
// Owns its own OpenAL sources (outside the emitter/target machinery, which can only start and stop on a frame
// boundary and streams everything in 400 ms blocks). Each shot of a burst appends one block of the loop file to a
// queued source, so the rhythm is set by the sample count of the blocks and the end of the burst falls exactly on
// a block boundary. Starts are scheduled on the OpenAL device clock (AL_SOFT_source_start_delay).

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <deque>
#include <mutex>

class CSoundRender_CyclicVoice
{
public:
	CSoundRender_CyclicVoice();
	~CSoundRender_CyclicVoice();

	// (Re)create the voice on the current device/context. efx_slot is the global reverb slot (0 = no EFX).
	void init(ALCdevice* dev, ALuint efx_slot);
	void destroy();
	bool ready() const { return m_ready; }

	void on_event(const SCyclicEvent& e); // game thread (weapon code)
	void update(float dt);                // sound task, once per frame
	void pause(bool val);
	void stop_all();

private:
	typedef s64 clk_t; // OpenAL device clock, nanoseconds

	// -- decoded samples ---------------------------------------------------------------------------------------
	struct SPcm
	{
		bool ok = false;
		int channels = 1;
		int rate = 44100;
		u32 frames = 0;
		float base_volume = 1.f;
		float max_ai_dist = 300.f;
		xr_vector<s16> pcm;
		ALuint whole = 0;                         // the full file as one buffer (start / end samples)
		xr_map<u64, ALuint> slices;               // (first_frame << 32 | frame_count) -> buffer
	};

	struct SEnv // gain envelope applied on top of the slot's base gain
	{
		bool active = false;
		bool fade_out = true;
		clk_t t0 = 0;
		clk_t dur = 0;
		int curve = 1; // 0 linear, 1 equal-power
		float value_before = 1.f; // gain before t0 (fade-in sources start silent)
	};

	struct SQ
	{
		ALuint buf;
		u32 samples;
	};

	struct SSlot
	{
		ALuint src = 0;
		bool in_use = false;       // owned by a role or still playing a tail
		clk_t play_clk = 0;        // scheduled start of what is queued
		clk_t release_after = 0;   // slot may be recycled once stopped and past this time
		float gain = 1.f;          // pcm base volume * effects volume
		float pitch = 1.f;
		int rate = 44100;
		SEnv env;
		bool stop_when_env_done = false;
		std::deque<SQ> q;
		u32 pending = 0;           // samples currently queued
		u32 stamp = 0;             // for stealing the oldest
	};

	enum { POOL = 16 };
	SSlot m_slot[POOL];
	int m_pool = 0;

	struct SSession
	{
		enum EState { Idle, Start, Loop, Ending } state = Idle;
		CObject* owner = nullptr;
		int ai_type = 0;
		int shots = 0;
		clk_t last_shot_clk = 0;
		double ivl_ema_ns = 0.0;
		double ivl_hist[5] = {};
		int ivl_count = 0;
		clk_t lat_ns = 0;          // pipeline delay in force for this burst
		int s_start = -1, s_loop = -1, s_end = -1;
		bool loop_started = false;
		float base_pitch = 1.f;
		float loop_sample_rpm = 600.f;
		shared_str loop_name;
		SPcm* loop_pcm = nullptr;
		int loop_shots = 0;
		SPcm* trans_pcm = nullptr;
		float trans_sample_rpm = 600.f;
		int trans_shots = 0;
		int trans_left = 0;
		int trans_idx = 0;
		clk_t loop_target_ns = 0;  // wanted amount of audio already queued when a shot arrives
		bool cut_pending = false;
		clk_t cut_clk = 0;
		SCyclicEvent cut_cfg;
		SCyclicEvent last_e;
		clk_t cleanup_clk = 0;
		float cycle_ns = 0.f;
	} m_s;

	// -- helpers -----------------------------------------------------------------------------------------------
	clk_t now_ns();                 // device clock, refined (see .cpp)
	clk_t latency_ns() const;       // effective pipeline delay: never below ~1.3 frame
	float effects_volume() const;
	SPcm* get_pcm(const shared_str& name);
	bool load_pcm(const char* name, SPcm& out);
	ALuint whole_buffer(SPcm& p);
	ALuint slice_buffer(SPcm& p, u32 first, u32 count);
	void free_pcm_all();

	int acquire_slot();
	void reset_slot(int si);
	void release_slot(int si);
	void apply_gain(SSlot& s, clk_t now);
	void set_env(SSlot& s, bool fade_out, clk_t t0, float dur_s, int curve, float value_before);
	void play_single(int si, SPcm* p, clk_t at, float pitch);
	void queue_slice(int si, SPcm* p, float sample_rpm, int nshots, int idx);
	void reclaim(int si);
	double remaining_ns(int si, clk_t now);
	void ensure_playing(int si, clk_t now);

	void on_begin(const SCyclicEvent& e);
	void on_shot(const SCyclicEvent& e);
	void on_release(const SCyclicEvent& e);
	void on_abort();
	void on_jam(const SCyclicEvent& e);
	void start_loop(const SCyclicEvent& e, clk_t now, int first_block = 0);
	void append_block(const SCyclicEvent& e, clk_t now);
	void schedule_end(const SCyclicEvent& e, clk_t now, bool immediate);
	void fire_ai(const SCyclicEvent& e, SPcm* p);
	void servo(const SCyclicEvent& e, clk_t now, double remaining_before_ns);
	void stop_slot_now(int si);

	void dbg(const char* fmt, ...) const;

	// device-clock refinement: the clock only advances once per mixer step (~10 ms), so it is paired with a
	// high-resolution timer and the offset between the two is the maximum seen over the last seconds
	enum { CLK_RING = 256 };
	s64 m_clk_r[CLK_RING] = {};
	s64 m_clk_off[CLK_RING] = {};
	int m_clk_n = 0;
	int m_clk_head = 0;
	float m_frame_ms = 8.3f; // smoothed frame time of the sound update

	bool m_ready = false;
	bool m_paused = false;
	u32 m_stamp = 0;
	ALCdevice* m_dev = nullptr;
	ALuint m_efx_slot = 0;
	xr_unordered_map<xr_string, SPcm*> m_pcm;
	size_t m_pcm_bytes = 0;
	std::mutex m_lock;

	// OpenAL Soft extensions (loaded at init)
	LPALSOURCEPLAYATTIMESOFT m_alSourcePlayAtTimeSOFT = nullptr;
	LPALCGETINTEGER64VSOFT m_alcGetInteger64vSOFT = nullptr;
};
